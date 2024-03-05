/**
 * @file vlc_access9P_plugin.c
 * @brief VLC access module for the 9P protocol
 */
#define MODULE_STRING "access9P"

#define PATH_MAX   (512)

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
// #include <vlc_tick.h>

/* Internal state for an instance of the module */
struct access_sys_t {
	char              *url;
	char              *fileservername;
	char              *filename;
	uint64_t           size;
	int                isfile;
	int                isaddr;
	int                fileserverfd;
	IxpClient         *client;
	IxpCFid           *fileserverfid;
};

static ssize_t Read(stream_t *, void *, size_t);
static int Seek(stream_t *, uint64_t);
static int Control(stream_t *, int, va_list);
static void seturl(stream_t *, char *);
static int open_9pconnection(stream_t *p_access);
static void close_9pconnection(stream_t *p_access);

/**
 * Init the input module
 */
static int
Open(vlc_object_t * obj)
{
	stream_t *p_access = (stream_t *) obj;
    const char *psz_url = p_access->psz_url;
	/* Allocate internal state */
	struct access_sys_t *p_sys = calloc(1, sizeof(*p_sys));
	if (unlikely(p_sys == NULL)) return VLC_ENOMEM;
	p_access->p_sys = p_sys;
	p_sys->url = (char*)psz_url + strlen("9p://");
	msg_Info(p_access, "opening %s ...", p_sys->url);
	seturl(p_access, p_sys->url);
	if (open_9pconnection(p_access) == -1) goto error;
    /* Set up p_access */
    p_access->pf_read = Read;
    p_access->pf_control = Control;
	msg_Info(p_access, "enabling seek: %s", p_sys->size ? "true" : "false");
    if (p_sys->size) {
		p_access->pf_seek = Seek;
	} else {
		p_access->pf_seek = NULL;
	}
	return VLC_SUCCESS;
 error:
	free(p_sys);
	return VLC_EGENERIC;
}

/**
 * Terminate the input module
 */
static void
Close(vlc_object_t *obj)
{
	stream_t *p_access = (stream_t *) obj;
	access_sys_t *p_sys = p_access->p_sys;
	msg_Info(p_access, "closing %s/%s ...", p_sys->url, p_sys->filename);
	close_9pconnection(p_access);
	/* Free internal state */
	// msg_Info(p_access, "freeing p_sys->url ...");
	// free(p_sys->url);
	// p_sys->url = NULL;
	// msg_Info(p_access, "freeing p_sys ...");
	free(p_sys);
	msg_Info(p_access, "closed.");
}


ssize_t
Read(stream_t *p_access, void *p_buffer, size_t i_len)
{
	access_sys_t *p_sys = p_access->p_sys;
    // if (p_sys->b_eof)
        // return 0;
	// if (p_sys->stream == NULL)
	// return 0;
	return ixp_read(p_sys->fileserverfid, p_buffer, i_len);
}


int
Seek(stream_t *p_access, uint64_t i_pos)
{
	msg_Info(p_access, "seeking to pos: %ld", i_pos);
	access_sys_t *p_sys = p_access->p_sys;
	p_sys->fileserverfid->offset = i_pos;
	return VLC_SUCCESS;
}


int
Control(stream_t *p_access, int i_query, va_list args)
{
	access_sys_t *p_sys = p_access->p_sys;
	bool *pb_bool;
	int64_t *pi_64;
	switch (i_query) {
	case STREAM_CAN_SEEK:
	// case STREAM_CAN_FASTSEEK:
		pb_bool = va_arg(args, bool *);
		if (p_sys->size == 0) {
			*pb_bool = false;
		} else {
			*pb_bool = true;
		}
		break;
	case STREAM_CAN_FASTSEEK:
		pb_bool = va_arg(args, bool *);
		*pb_bool = false;
		break;
	case STREAM_CAN_PAUSE:
	case STREAM_CAN_CONTROL_PACE:
		pb_bool = va_arg(args, bool *);
		if (p_sys->size == 0) {
			*pb_bool = false;
		} else {
			*pb_bool = true;
		}
		break;
    case STREAM_GET_PTS_DELAY:
            pi_64 = va_arg( args, int64_t * );
            *pi_64 = INT64_C(1000) * var_InheritInteger( p_access, "network-caching" );
    /// Current versions of vlc ...
        // *va_arg(args, vlc_tick_t *) =
            // VLC_TICK_FROM_MS(var_InheritInteger(p_access,"network-caching"));
        break;
    case STREAM_GET_SIZE:
        return VLC_EGENERIC;
        if (p_sys->size == 0) return VLC_EGENERIC;
        *va_arg( args, uint64_t * ) = p_sys->size;
        break;
    case STREAM_SET_PAUSE_STATE:
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
    add_shortcut("9P", "9p")
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_ACCESS)
vlc_module_end()


static int
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


static void
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


static void
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


static int
open_9pconnection(stream_t *p_access)
{
	access_sys_t *p_sys = p_access->p_sys;
	msg_Info(p_access, "opening 9P connection to: %s ...", p_sys->url);
	if (p_sys->isfile) {
		msg_Info(p_access, "input is a file, nothing to do");
		return 0;
	}
	p_sys->client = ixp_mount(p_sys->fileservername);
	if (!p_sys->client) {
		msg_Err(p_access, "failed to open 9P connection");
		return 1;
	}
	msg_Info(p_access, "stat 9P file: %s ...", p_sys->filename);
	Stat *dstat = ixp_stat(p_sys->client, p_sys->filename);
	if (dstat == NULL) {
		msg_Err(p_access, "failed to stat '%s', assuming non-seekable stream ...",
		  p_sys->filename);
	} else {
		p_sys->size = dstat->length;
		msg_Info(p_access, "size of '%s': %ld\n", p_sys->filename, p_sys->size);
		ixp_freestat(dstat);
	}
	msg_Info(p_access, "opening 9P file: %s ...", p_sys->filename);
	p_sys->fileserverfid = ixp_open(p_sys->client, p_sys->filename, P9_OREAD);
	if (!p_sys->fileserverfid) {
		msg_Err(p_access, "failed to open file");
		return 1;
	}
	return 0;
}


static void
close_9pconnection(stream_t *p_access)
{
	access_sys_t *p_sys = p_access->p_sys;
	if (p_sys->fileserverfid) {
		ixp_close(p_sys->fileserverfid);
	}
	if (p_sys->isfile) {
		msg_Info(p_access, "input is a file, nothing to do");
	} else {
		ixp_unmount(p_sys->client);
	}
}
