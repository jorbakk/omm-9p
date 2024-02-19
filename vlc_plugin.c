/**
 * @file hello.c
 * @brief Hello world interface VLC module example
 */
#define MODULE_STRING "access9P"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <u.h>
#include <time.h>  // posix std headers should be included between u.h and libc.h
#include <stdlib.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include <9pclient.h>

/* VLC core API headers */
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_access.h>
// #include <vlc_meta.h>

/* Internal state for an instance of the module */
struct access_sys_t {
	char              *url;
	char              *fileservername;
	char              *filename;
	int                isfile;
	int                isaddr;
	int                fileserverfd;
	CFid              *fileserverfid;
	CFsys             *fileserver;
};

static ssize_t Read(stream_t *, void *, size_t);
static int Seek(stream_t *, uint64_t);
static int Control(stream_t *, int, va_list);
void seturl(stream_t *, char *);

/**
 * Init the input module
 */
static int
Open(vlc_object_t * obj)
{
	stream_t *p_access = (stream_t *) obj;
    const char *psz_url = p_access->psz_url;
	/* Allocate internal state */
	struct access_sys_t *sys = malloc(sizeof(*sys));
	sys->url = psz_url + 5;
	if (unlikely(sys == NULL)) return VLC_ENOMEM;
	p_access->p_sys = sys;
	msg_Info(p_access, "opening %s ...", sys->url);
	seturl(p_access, sys->url);
	if (open_9pconnection(p_access) == -1) goto error;
    /* Set up p_access */
    p_access->pf_read = Read;
    p_access->pf_control = Control;
    p_access->pf_seek = Seek;
	return VLC_SUCCESS;
 error:
	free(sys);
	return VLC_EGENERIC;
}

/**
 * Terminate the input module
 */
static void
Close(vlc_object_t *obj)
{
	stream_t *p_access = (stream_t *) obj;
	struct access_sys_t *sys = p_access->p_sys;
	/* Free internal state */
	free(sys->url);
	free(sys);
	msg_Info(p_access, "closed %s!", sys->url);
}

ssize_t
Read(stream_t *p_access, void *p_buffer, size_t i_len)
{
	access_sys_t *p_sys = p_access->p_sys;
	// if (p_sys->stream == NULL)
	// return 0;

}

int
Seek(stream_t *p_access, uint64_t i_pos)
{
	(void)p_access;
	(void)i_pos;
	return VLC_EGENERIC;
}

int
Control(stream_t *p_access, int i_query, va_list args)
{
	access_sys_t *p_sys = p_access->p_sys;
	switch (i_query) {
		/* */
	case STREAM_CAN_SEEK:
	case STREAM_CAN_FASTSEEK:
		// pb_bool = va_arg(args, bool *);
		// *pb_bool = false;
		break;
	case STREAM_CAN_PAUSE:
	case STREAM_CAN_CONTROL_PACE:
		// pb_bool = va_arg(args, bool *);
		// *pb_bool = true;
		break;
	default:
		return VLC_EGENERIC;

	}
	return VLC_SUCCESS;
}


/* Module descriptor */
vlc_module_begin()
    set_shortname("9P access")
    set_description("Reading media data using the 9P protocol")
    set_capability("access", 60)
    set_callbacks(Open, Close)
    // ACCESS_SET_CALLBACKS(Read, NULL, Control, Seek)
    add_shortcut("9P", "9p")
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_ACCESS)
vlc_module_end()


int
parseurl(char *url, char **fileservername, char **filename, int *isaddr, int *isfile)
{
	char *pfileservername = nil;
	char *pfilename = nil;
	char *pbang = strchr(url, '!');
	char *pslash = strchr(url, '/');
	int fisaddr = 0;
	int fisfile = 0;
	if (pslash == nil) {
		if (pbang != nil) {
			return -1;
		}
		pfilename = url;
	}
	else if (pslash == url) {
		// Local file path that starts with '/'
		pfilename = url;
		fisfile = 1;
	}
	else {
		*pslash = '\0';
		pfileservername = url;
		pfilename = pslash + 1;
		if (pbang != nil) {
			fisaddr = 1;
		}
	}
	*fileservername = pfileservername;
	*filename = pfilename;
	*isaddr = fisaddr;
	*isfile = fisfile;
	return 0;
}


void
setstr(char **str, char *instr, int ninstr)
{
	if (*str) {
		free(*str);
	}
	if (ninstr == 0) {
		ninstr = strlen(instr);
	}
	*str = malloc(ninstr + 1);
	memcpy(*str, instr, ninstr);
	*(*str + ninstr) = '\0';
}


void
seturl(stream_t *p_access, char *url)
{
	access_sys_t *p_sys = p_access->p_sys;
	char *s, *f;
	int ret = parseurl(url, &s, &f, &p_sys->isaddr, &p_sys->isfile);
	if (p_sys->isfile) {
		msg_Info(p_access, "input is file, setting url to %s", url);
		setstr(&p_sys->filename, url, 0);
		return;
	}
	if (ret == -1) {
		msg_Err(p_access, "failed to parse url %s", url);
		p_sys->fileservername = nil;
		p_sys->filename = nil;
		return;
	}
	/* setstr(&p_sys->fileservername, DEFAULT_SERVER_NAME, 0); */
	setstr(&p_sys->fileservername, s, 0);
	setstr(&p_sys->filename, f, 0);
}


int
clientdial(stream_t *p_access)
{
	access_sys_t *p_sys = p_access->p_sys;
	msg_Info(p_access, "dialing address: %s ...", p_sys->fileservername);
	if ((p_sys->fileserverfd = dial(p_sys->fileservername, nil, nil, nil)) < 0)
		return -1;
		/* sysfatal("dial: %r"); */
	msg_Info(p_access, "mounting file descriptor ...");
	if ((p_sys->fileserver = fsmount(p_sys->fileserverfd, nil)) == nil)
		return -1;
		/* sysfatal("fsmount: %r"); */
	return 0;
}


int
clientmount(stream_t *p_access)
{
	access_sys_t *p_sys = p_access->p_sys;
	msg_Info(p_access, "mounting address: %s ...", p_sys->fileservername);
	if ((p_sys->fileserver = nsmount(p_sys->fileservername, nil)) == nil)
		return -1;
	msg_Info(p_access, "mounting address: %s success.", p_sys->fileservername);
	return 0;
}


int
open_9pconnection(stream_t *p_access)
{
	access_sys_t *p_sys = p_access->p_sys;
	msg_Info(p_access, "opening 9P connection ...");
	if (p_sys->isfile) {
		msg_Info(p_access, "input is a file, nothing to do");
		return 0;
	}
	int ret;
	if (!p_sys->fileserver) {
		if (p_sys->isaddr) {
			/* p_sys->fileserver = clientdial(p_sys->fileservername); */
			ret = clientdial(p_access);
		}
		else {
			/* p_sys->fileserver = clientmount(p_sys->fileservername); */
			ret = clientmount(p_access);
		}
		/* p_sys->fileserver = clientdial("tcp!localhost!5640"); */
		/* p_sys->fileserver = clientdial("tcp!192.168.1.85!5640"); */
		if (ret == -1) {
			msg_Err(p_access, "failed to open 9P connection");
			return ret;
		}
	}
	msg_Info(p_access, "opening 9P file: %s ...", p_sys->filename);
	CFid *fid = fsopen(p_sys->fileserver, p_sys->filename, OREAD);
	if (fid == nil) {
		// blank_window(p_access);
		return -1;
	}
	p_sys->fileserverfid = fid; 
	return 0;
}


void
close_9pconnection(stream_t *p_access)
{
	access_sys_t *p_sys = p_access->p_sys;
	if (p_sys->isfile) {
		msg_Info(p_access, "input is a file, nothing to do");
	}
	if (p_sys->fileserverfid) {
		fsclose(p_sys->fileserverfid);
	}
}
