#define IXP_NO_P9_
#define IXP_P9_STRUCTS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ixp.h>


#define fatal(...) ixp_eprint("ixpc: fatal: " __VA_ARGS__); \

static IxpClient *serve, *render;
static char *serve_addr = "tcp!127.0.0.1!2001";
static char *render_addr = "tcp!127.0.0.1!2002";


static int
xls(int argc, char *argv[])
{
	(void)argc; (void)argv;
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
	/// FIXME metapath should be a dynamic string
	char metapath[64] = {0};
	for(i = 0; i < stat_size; i++) {
		/// Entry is a file, we're looking for dirs, only
		if ((stat[i].mode & P9_DMDIR) == 0) continue;
		sprintf(metapath, "/%s/meta", stat[i].name);
		fid = ixp_open(serve, metapath, P9_OREAD);
		if(fid == NULL) {
			fprintf(stderr, "failed to open '%s', skipping ...\n", metapath);
			continue;
		}
		int pos = 0;
		/// FIXME meta should be a dynamic string
		char meta[64] = {0};
		buf = ixp_emalloc(fid->iounit);
		while((count = ixp_read(fid, buf, fid->iounit)) > 0) {
			memcpy(meta + pos, buf, count);
			pos += count;
		}
		ixp_close(fid);
		free(buf);
		if(count == -1) {
			fprintf(stderr, "failed to read dir entry '%s': %s\n", metapath, ixp_errbuf());
			goto cleanup;
		}
		fprintf(stdout, "%s - %s\n", stat[i].name, meta);
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
write_ctl_cmdbuf(char *buf)
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


static int
xset(int argc, char *argv[])
{
	char *arg;
	if (argc == 2) {
		arg = argv[1];
	}
	else {
		fprintf(stderr, "usage: %s <media id>\n", argv[0]);
		return 1;
	}
	/// FIXME buf should be a dynamic string
	char buf[64] = {0};
	sprintf(buf, "%s 9p://%s/%s/data", argv[0], serve_addr, arg);
	return write_ctl_cmdbuf(buf);
}


static int
xnoparms(int argc, char *argv[])
{
	if (argc >= 2) {
		fprintf(stderr, "usage: %s\n", argv[0]);
		return 1;
	}
	return write_ctl_cmdbuf(argv[0]);
}


struct exectab {
	char *cmd;
	int (*fn)(int, char**);
} etab[] = {
	{"ls", xls},
	{"set", xset},
	{"play", xnoparms},
	{"stop", xnoparms},
	{0, 0}
};


int
main(int argc, char *argv[]) {
	char *cmd;
	struct exectab *tab;
	int ret = 1;
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
