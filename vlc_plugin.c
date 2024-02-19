/**
 * @file vlc_plugin.c
 * @brief VLC access module for 9P protocol
 */
#define MODULE_STRING "access9P"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define IXP_NO_P9_
#define IXP_P9_STRUCTS
#include <ixp.h>

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
	IxpClient         *client;
	IxpCFid           *fileserverfid;
};

static ssize_t Read(stream_t *, void *, size_t);
static int Seek(stream_t *, uint64_t);
static int Control(stream_t *, int, va_list);
void seturl(stream_t *, char *);
int open_9pconnection(stream_t *p_access);
void close_9pconnection(stream_t *p_access);

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
	close_9pconnection(p_access);
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
	return ixp_read(p_sys->fileserverfid, p_buffer, i_len);
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
	char *pfileservername = NULL;
	char *pfilename = NULL;
	char *pbang = strchr(url, '!');
	char *pslash = strchr(url, '/');
	int fisaddr = 0;
	int fisfile = 0;
	if (pslash == NULL) {
		if (pbang != NULL) {
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
		if (pbang != NULL) {
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
	} else {
		msg_Info(p_access, "server address is: %s", s);
	}
	if (ret == -1) {
		msg_Err(p_access, "failed to parse url %s", url);
		p_sys->fileservername = NULL;
		p_sys->filename = NULL;
		return;
	}
	/* setstr(&p_sys->fileservername, DEFAULT_SERVER_NAME, 0); */
	setstr(&p_sys->fileservername, s, 0);
	setstr(&p_sys->filename, f, 0);
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
	p_sys->client = ixp_mount(p_sys->fileservername);
	if (!p_sys->client) {
		msg_Err(p_access, "failed to open 9P connection");
		return 1;
	}
	msg_Info(p_access, "opening 9P file: %s ...", p_sys->filename);
	p_sys->fileserverfid = ixp_open(p_sys->client, p_sys->filename, P9_OREAD);
	if (!p_sys->fileserverfid) {
		msg_Err(p_access, "failed to open file");
		return 1;
	}
	return 0;
}


void
close_9pconnection(stream_t *p_access)
{
	access_sys_t *p_sys = p_access->p_sys;
	if (p_sys->isfile) {
		msg_Info(p_access, "input is a file, nothing to do");
	} else {
		ixp_unmount(p_sys->client);
	}
	if (p_sys->fileserverfid) {
		ixp_close(p_sys->fileserverfid);
	}
}
