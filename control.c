#define _XOPEN_SOURCE 700
#define IXP_NO_P9_
#define IXP_P9_STRUCTS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ixp.h>

/// Length of string: "tcp!ip!port"
#define ADDR_MAX   (64)
/// Length of string: "<objid>/meta"
#define PATH_MAX   (128)
/// Length of string: "put <mrl>"
#define MRL_MAX    (128)
/// Size of obj meta data buffer
#define META_MAX   (1024)
/// Length of fav command
#define FAV_MAX    (128)
// #define COL_SEP    ("|")
#define COL_SEP    ("│")

#define fatal(...) ixp_eprint("ixpc: fatal: " __VA_ARGS__); \

static IxpClient *serve, *render;
/// Set the IP address of the omm servers via environment variable
static char *omm_ip_envar = "OMM_ADDRESS";
static char *serve_ip_envar = "OMM_SERVE_ADDRESS";
static char *render_ip_envar = "OMM_RENDER_ADDRESS";
/// Default IP address of omm servers
static char *omm_ip = "127.0.0.1";
static char *serve_ip = NULL;
static char *render_ip = NULL;
static int serve_port = 2001;
static int render_port = 2002;
/// FIXME server and renderer addresses should be dynamic arrays
static char serve_addr[ADDR_MAX] = {0};
static char render_addr[ADDR_MAX] = {0};

enum {
	MET_TYPE = 0,
	MET_FMT,
	MET_DUR,
	MET_ORIG,
	MET_ALBUM,
	MET_TRACK,
	MET_TITLE,
	MET_PATH,
	MET_CNT,
};


struct time {
	uint8_t h, m, s, ms;
};


void msec2time(struct time *t, uint64_t ms)
{
	t->s = ms / 1e3f;
	t->ms = ms % 1000;
	t->m = t->s / 60;
	t->s = t->s % 60;
	t->h = t->m / 60;
	t->m = t->m % 60;
	t->h = t->h % 60;
}


int write_sqry_cmdbuf(char *buf);


static int
xls(int argc, char *argv[])
{
	if (argc == 1) {
		write_sqry_cmdbuf("");
	} else if (argc == 2) {
		write_sqry_cmdbuf(argv[1]);
	} else if (argc > 2) {
		fprintf(stderr, "usage: %s <search pattern>\n", argv[0]);
		return 1;
	}

	IxpMsg m;
	Stat *stat;
	IxpCFid *fid;
	char *file, *buf;
	int count, i;

	int stat_size = 0;
	int stat_capa = 16;
	int ret = 1;

	file = "/";
	stat = ixp_stat(serve, file);
	stat_size++;
	if(stat == NULL) {
		fprintf(stderr, "failed to stat file '%s': %s\n", file, ixp_errbuf());
		goto cleanup;
	}
	if((stat->mode & P9_DMDIR) == 0) {
		fprintf(stderr, "root is a file, skipping ...");
		goto cleanup;
	}
	ixp_freestat(stat);
	stat_size--;
	fid = ixp_open(serve, file, P9_OREAD);
	if(fid == NULL) {
		fprintf(stderr, "failed to open dir '%s': %s\n", file, ixp_errbuf());
		goto cleanup;
	}
	/// Read the root dir and stat each entry
	stat = ixp_emalloc(sizeof(*stat) * stat_capa);
	buf = ixp_emalloc(fid->iounit);
	while((count = ixp_read(fid, buf, fid->iounit)) > 0) {
		m = ixp_message(buf, count, MsgUnpack);
		while(m.pos < m.end) {
			if(stat_size == stat_capa) {
				stat_capa <<= 1;
				stat = realloc(stat, sizeof(*stat) * stat_capa);
			}
			ixp_pstat(&m, &stat[stat_size++]);
		}
	}
	ixp_close(fid);
	free(buf);
	if(count == -1) {
		fprintf(stderr, "failed to read dir '%s': %s\n", file, ixp_errbuf());
		goto cleanup;
	}
	/// Print out the meta data of each root dir entry
	char path[PATH_MAX] = {0};
	for(i = 0; i < stat_size; i++) {
		/// Entry is a file, we're looking for dirs, only
		if ((stat[i].mode & P9_DMDIR) == 0) continue;
		/// Stat data file
		sprintf(path, "/%s/data", stat[i].name);
		Stat *dstat = ixp_stat(serve, path);
		if(dstat == NULL) {
			fprintf(stderr, "failed to stat '%s', skipping ...\n", path);
			continue;
		}
		uint64_t fsize = dstat->length;
		ixp_freestat(dstat);
		/// Read meta file
		sprintf(path, "/%s/meta", stat[i].name);
		fid = ixp_open(serve, path, P9_OREAD);
		fprintf(stderr, "opening path: %s ...\n", path);
		if(fid == NULL) {
			fprintf(stderr, "failed to open '%s', skipping ...\n", path);
			continue;
		}
		int meta_len = 0;
		char meta[META_MAX] = {0};
		buf = ixp_emalloc(fid->iounit);
		while((count = ixp_read(fid, buf, fid->iounit)) > 0) {
			memcpy(meta + meta_len, buf, count);
			meta_len += count;
		}
		ixp_close(fid);
		free(buf);
		if(count == -1) {
			fprintf(stderr, "failed to read from '%s': %s\n", path, ixp_errbuf());
			goto cleanup;
		}
		if(count == 0) {
			fprintf(stderr, "'%s' is empty\n", path);
			goto cleanup;
		}
		char *metargs[MET_CNT] = {0};
		metargs[0] = meta;
		char *ma = meta;
		// char sep = '\0';
		char sep = '\1';
		// char sep = '@';
		for (int m = 1; m < MET_CNT; ++m) {
			ma = memchr(ma, sep, meta_len - (ma - meta));
			*(ma) = '\0';
			ma++;
			metargs[m] = ma;
		}
		struct time t;
		uint64_t ms = atol(metargs[MET_DUR]);
		msec2time(&t, ms);
		// fprintf(stdout, "%2s "COL_SEP" %4.1f MB "COL_SEP" %02d:%02d:%02d "COL_SEP" %16s "COL_SEP" %s\n",
		fprintf(stdout, "%2s | %4.1f MB | %02d:%02d:%02d | %16s | %s\n",
			stat[i].name, fsize / 1e6,
			t.h, t.m, t.s,
			metargs[MET_ORIG], metargs[MET_TITLE]);
	}
	ret = 0;
cleanup:
	for(i = 0; i < stat_size; i++) {
		ixp_freestat(&stat[i]);
	}
	return ret;
}


void
write_buf(IxpCFid *fid, char *buf, int len)
{
	/// FIXME ixp_write() seems to write even though count == 0, 
	///       ... also, we must write one more byte ...
	len++;
	int pos = 0, count = 0;
	while (pos < len && (count = ixp_write(fid, buf + pos, len - pos)) > 0) {
		pos += count;
	}
}


int
write_rctl_cmdbuf(char *buf)
{
	char *file = "/ctl";
	IxpCFid *fid = ixp_open(render, file, P9_OWRITE);
	if(fid == NULL) {
		fprintf(stderr, "failed to open ommrender ctl file: %s\n", ixp_errbuf());
		return 1;
	}
	write_buf(fid, buf, strlen(buf));
	ixp_close(fid);
	return 0;
}


int
write_sqry_cmdbuf(char *buf)
{
	char *file = "/query";
	IxpCFid *fid = ixp_open(serve, file, P9_OWRITE);
	if(fid == NULL) {
		fprintf(stderr, "failed to open ommserve query file: %s\n", ixp_errbuf());
		return 1;
	}
	write_buf(fid, buf, strlen(buf));
	ixp_close(fid);
	return 0;
}


int
write_sctl_cmdbuf(char *buf)
{
	char *file = "/ctl";
	IxpCFid *fid = ixp_open(serve, file, P9_OWRITE);
	if(fid == NULL) {
		fprintf(stderr, "failed to open ommserve ctl file: %s\n", ixp_errbuf());
		return 1;
	}
	write_buf(fid, buf, strlen(buf));
	ixp_close(fid);
	return 0;
}


static int
xnoparms(int argc, char *argv[])
{
	if (argc >= 2) {
		fprintf(stderr, "usage: %s\n", argv[0]);
		return 1;
	}
	return write_rctl_cmdbuf(argv[0]);
}


static int
xput(int argc, char *argv[])
{
	char *arg;
	if (argc == 2) {
		arg = argv[1];
	}
	else {
		fprintf(stderr, "usage: %s <media id>\n", argv[0]);
		return 1;
	}
	char buf[MRL_MAX] = {0};
	sprintf(buf, "%s 9p://%s/%s/data", argv[0], serve_addr, arg);
	return write_rctl_cmdbuf(buf);
}


static int
xseek(int argc, char *argv[])
{
	char *arg;
	if (argc == 2) {
		arg = argv[1];
	}
	else {
		fprintf(stderr, "usage: %s <position percentage>\n", argv[0]);
		return 1;
	}
	char buf[MRL_MAX] = {0};
	sprintf(buf, "%s %s", argv[0], arg);
	return write_rctl_cmdbuf(buf);
}


static int
xvol(int argc, char *argv[])
{
	char *arg;
	if (argc == 2) {
		arg = argv[1];
	}
	else {
		fprintf(stderr, "usage: %s <volume percentage>\n", argv[0]);
		return 1;
	}
	char buf[MRL_MAX] = {0};
	sprintf(buf, "%s %s", argv[0], arg);
	return write_rctl_cmdbuf(buf);
}


static int
xfav(int argc, char *argv[])
{
	char buf[FAV_MAX] = {0};
	if (argc == 4) {
		sprintf(buf, "%s %s %s %s", argv[0], argv[1], argv[2], argv[3]);
	} else if (argc == 3) {
		sprintf(buf, "%s %s %s", argv[0], argv[1], argv[2]);
	} else if (argc == 2) {
		sprintf(buf, "%s %s", argv[0], argv[1]);
	} else {
		fprintf(stderr, "usage:\n  %s add|del <favlist id> <media id>\n  %s set <favlist id>\n",
		  argv[0], argv[0]);
		return 1;
	}
	return write_sctl_cmdbuf(buf);
}


struct exectab {
	char *cmd;
	int (*fn)(int, char**);
} etab[] = {
	{"ls", xls},
	{"put", xput},
	{"play", xnoparms},
	{"stop", xnoparms},
	{"pause", xnoparms},
	{"fav", xfav},
	{"seek", xseek},
	{"vol", xvol},
	{0, 0}
};


int
main(int argc, char *argv[]) {
	char *cmd;
	struct exectab *tab;
	int ret = 1;
	char *oe = getenv(omm_ip_envar);
	if (oe) omm_ip = oe;
	serve_ip = getenv(serve_ip_envar);
	if (!serve_ip) serve_ip = omm_ip;
	render_ip = getenv(render_ip_envar);
	if (!render_ip) render_ip = omm_ip;
	sprintf(serve_addr, "tcp!%s!%d", serve_ip, serve_port);
	sprintf(render_addr, "tcp!%s!%d", render_ip, render_port);
	serve = ixp_mount(serve_addr);
	if(serve == NULL) {
		fatal("ommserve not available, %s\n", ixp_errbuf());
	}
	render = ixp_mount(render_addr);
	if(render == NULL) {
		fprintf(stderr, "ommrender not available, %s\n", ixp_errbuf());
	}
	if (argc == 1) {
		cmd = "ls";
	} else {
		cmd = argv[1];
	}
	for(tab = etab; tab->cmd; tab++) {
		if(strcmp(cmd, tab->cmd) == 0) break;
	}
	if (tab->cmd == NULL) {
		fprintf(stderr, "unknown command '%s'\n", cmd);
	} else {
		ret = tab->fn(argc - 1, argv + 1);
	}
	ret = 0;
	ixp_unmount(serve);
	if (render) {
		ixp_unmount(render);
	}
	return ret;
}
