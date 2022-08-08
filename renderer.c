/*
 * Copyright 2022 Jörg Bakker
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */


// TODO:
// 1. Fixes
// - start / stop different media streams in sequence
//   - audio only stream doesn't stop first time
// - dvb life streams need too long to start rendering, service queue on server gets full
// - memory leaks
// - seek
// - blank screen on stop / eof
// - responsiveness to keyboard input
// 2. AV sync
// - decrease video picture display rate variation further
// - remove audio delay (... if there's any ... caused by samples in sdl queue?!)
// - add video-only (for videos with or w/o audio) and fix audio-only video playback
// 3. Query renderer info (current position, media length, renderer state, audio volume) from 9P server
// 4. Display single still images
// 5. Refactoring / testing
// - allow video scaling not only in decoder thread but also in presenter thread
// - test keyboard / server input combinations (fuzz testing ...)
// 6. Experiment with serving video and audio output channels via the 9P server
// 7. Build renderer into drawterm-av

// Thread layout:
//   main_thread (event loop)
//   -> 9P command server thread/proc
//   -> decoder_thread
//      -> presenter_thread reads from audio and video channel


#include <u.h>
#include <time.h>  // posix std headers should be included between u.h and libc.h
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include <9pclient.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>

#include <SDL2/SDL.h>


#define LOG(...) printloginfo(); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");

static struct timespec curtime;
static FILE *audio_out;

void printloginfo(void)
{
	long ms;  // Milliseconds
	time_t s; // Seconds
	pid_t tid;
	/* tid = syscall(SYS_gettid); */
	tid = threadid();
	timespec_get(&curtime, TIME_UTC);
	/* clock_gettime(CLOCK_REALTIME, &curtime); */
	s  = curtime.tv_sec;
	ms = round(curtime.tv_nsec / 1.0e6); // Convert nanoseconds to milliseconds
	if (ms > 999) {
	    s++;
	    ms = 0;
	}
	fprintf(stderr, "%"PRIdMAX".%03ld %d│ ", (intmax_t)s, ms, tid);
}


#define _DEBUG_ 1
#define DEFAULT_SERVER_NAME ("ommserver")
#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000
#define MAX_CMD_STR_LEN 256
#define MAX_COMMANDQ_SIZE (5)
#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)
#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 1.0
#define SAMPLE_CORRECTION_PERCENT_MAX 10
#define AUDIO_DIFF_AVG_NB 20
#define VIDEO_PICTURE_QUEUE_SIZE 1
/* #define DEFAULT_AV_SYNC_TYPE AV_SYNC_AUDIO_MASTER */
#define DEFAULT_AV_SYNC_TYPE AV_SYNC_EXTERNAL_MASTER
#define THREAD_STACK_SIZE 1024 * 1024 * 10
#define avctxBufferSize 8192 * 10


typedef struct RendererCtx
{
	// State machine
	int                renderer_state;
	int                next_renderer_state;
	int                quit;
	// Input file name, plan 9 file reference, os window
	char              *url;
	char              *fileservername;
	char              *filename;
	int                isfile;
	int                isaddr;
	int                fileserverfd;
	CFid              *fileserverfid;
	CFsys             *fileserver;
	AVIOContext       *io_ctx;
	AVFormatContext   *format_ctx;
	AVCodecContext    *current_codec_ctx;
	Channel           *cmdq;
	int                screen_width;
	int                screen_height;
	int                window_width;
	int                window_height;
	// Threads
	int                server_tid;
	int                decoder_tid;
	int                presenter_tid;
	// Audio stream
	int                audio_stream;
	AVCodecContext    *audio_ctx;
	Channel           *audioq;
	SDL_AudioDeviceID  audio_devid;
	int                audio_only;
	uint8_t            mixed_audio_buf[MAX_AUDIO_FRAME_SIZE];
	unsigned int       audio_buf_size;
	unsigned int       audio_buf_index;
	AVFrame            audio_frame;
	AVPacket           audio_pkt;
	int                audio_idx;
	int                audio_out_channels;
	double             current_video_time;
	double             previous_video_time;
	int64_t            audio_start_rt;
	AVRational         audio_timebase;
	double             audio_tbd;
	int                audio_vol;
	SwrContext        *swr_ctx;
	// Video stream
	SDL_Window        *sdl_window;
	SDL_Texture       *sdl_texture;
	SDL_Renderer      *sdl_renderer;
	int                video_stream;
	double             frame_rate;
	double             frame_duration;
	AVCodecContext    *video_ctx;
	Channel           *pictq;
	struct SwsContext *yuv_ctx;
	struct SwsContext *rgb_ctx;
	SDL_AudioSpec      specs;
	int                video_idx;
	AVFrame           *frame_rgb;
	uint8_t           *rgbbuffer;
	uint8_t           *yuvbuffer;
	int                w, h, aw, ah;
	SDL_Rect           blit_copy_rect;
	AVRational         video_timebase;
	double             video_tbd;
	// Seeking
	int	               seek_req;
	int	               seek_flags;
	int64_t            seek_pos;
	int                frame_fmt;
} RendererCtx;

typedef struct VideoPicture
{
	AVFrame    *frame;
	uint8_t    *rgbbuf;
	int         linesize;
	/* uint8_t     *planes[AV_NUM_DATA_POINTERS]; */
	/* int         linesizes[AV_NUM_DATA_POINTERS]; */
	uint8_t    *planes[4];
	int         linesizes[4];
	int         width;
	int         height;
	int         pix_fmt;
	double      pts;
	int         idx;
} VideoPicture;

typedef struct AudioSample
{
	/* uint8_t			 sample[MAX_AUDIO_FRAME_SIZE]; */
	uint8_t	   *sample;
	int         size;
	int         idx;
	double      pts;
	double      duration;
} AudioSample;


// State machine
#define NSTATE 8
typedef void (*state_func)(RendererCtx*);

void state_stop(RendererCtx* rctx);
void state_run(RendererCtx* rctx);
void state_idle(RendererCtx* rctx);
void state_load(RendererCtx* rctx);
void state_unload(RendererCtx* rctx);
void state_engage(RendererCtx* rctx);
void state_disengage(RendererCtx* rctx);
void state_exit(RendererCtx* rctx);

static state_func states[NSTATE] =
{
	state_stop,
	state_run,
	state_idle,
	state_load,
	state_unload,
	state_engage,
	state_disengage,
	state_exit,
};

enum
{
	STOP = 0,
	RUN,
	IDLE,
	LOAD,
	UNLOAD,
	ENGAGE,
	DISENG,
	EXIT,

	RSTATE_STOP,
	RSTATE_PLAY,
	RSTATE_PAUSE,
	RSTATE_SEEK,
	RSTATE_QUIT,
};

typedef struct Command
{
	int         cmd;
	char       *arg;
	int         narg;
} Command;

#define NCMD 9
typedef void (*cmd_func)(RendererCtx*, char*, int);

void cmd_set(RendererCtx *rctx, char *arg, int narg);
void cmd_stop(RendererCtx *rctx, char *arg, int narg);
void cmd_play(RendererCtx *rctx, char *arg, int narg);
void cmd_pause(RendererCtx *rctx, char *arg, int narg);
void cmd_quit(RendererCtx *rctx, char *arg, int narg);
void cmd_seek(RendererCtx *rctx, char *arg, int narg);
void cmd_vol(RendererCtx *rctx, char *arg, int narg);

enum
{
	CMD_SET = 0,   // Set *next* url
	CMD_STOP,
	CMD_PLAY,
	CMD_PAUSE,
	CMD_QUIT,
	CMD_SEEK,      // Seek to second from start
	CMD_VOL,       // Set percent soft volume
	CMD_NONE,
	CMD_ERR,
};

enum
{
	READCMD_BLOCK = 0,
	READCMD_POLL,
};

enum
{
	KEEP_STATE = 0,
	CHANGE_STATE,
};

static cmd_func cmds[NCMD] =
{
	cmd_set,
	/* cmd_stop, */
	/* cmd_play, */
	/* cmd_pause, */
	nil,
	nil,
	nil,
	cmd_quit,
	cmd_seek,
	cmd_vol,
	nil,
	nil
};

// Transitions is the State matrix with dimensions [cmds x current state]
// Each entry is the *next* state when cmd is received in current state
// FIXME better ignore commands in states like LOAD, UNLOAD, ENGAGE, DISENG
static int transitions[NCMD][NSTATE-1] = // no entry for EXIT state needed
{
// Current state:
//   STOP,    RUN,     IDLE,    LOAD,    UNLOAD,  ENGAGE,  DISENG
// CMD_SET
	{STOP,    RUN,     IDLE,    LOAD,    UNLOAD,  ENGAGE,  DISENG},
// CMD_STOP
	{STOP,    UNLOAD,  UNLOAD,  STOP,    UNLOAD,  UNLOAD,  UNLOAD},
// CMD_PLAY
	{LOAD,    RUN,     ENGAGE,  LOAD,    UNLOAD,  ENGAGE,  ENGAGE},
// CMD_PAUSE
	{STOP,    DISENG,  ENGAGE,  DISENG,  UNLOAD,  DISENG,  DISENG},
// CMD_QUIT
	{EXIT,    EXIT,    EXIT,    EXIT,    EXIT,    EXIT,    EXIT},
// CMD_SEEK
	{STOP,    RUN,     IDLE,    LOAD,    UNLOAD,  ENGAGE,  DISENG},
// CMD_VOL
	{STOP,    RUN,     IDLE,    LOAD,    UNLOAD,  ENGAGE,  DISENG},
// CMD_NONE - unconditional straight transitions
	{STOP,    RUN,     IDLE,    RUN,     STOP,    RUN,     IDLE},
// CMD_ERR - error occured while running the state
	{STOP,    UNLOAD,  IDLE,    STOP,    STOP,    STOP,    STOP},
};


// Frame, clock, and sample types
enum
{
	FRAME_FMT_PRISTINE,
	FRAME_FMT_RGB,
	FRAME_FMT_YUV,
};

enum
{
	// Sync to audio clock.
	AV_SYNC_AUDIO_MASTER,
	// Sync to video clock.
	AV_SYNC_VIDEO_MASTER,
	// Sync to external clock: the computer clock
	AV_SYNC_EXTERNAL_MASTER,
};

struct sample_fmt_entry {
    enum AVSampleFormat sample_fmt; const char *fmt_be, *fmt_le;
} sample_fmt_entries[] = {
    { AV_SAMPLE_FMT_U8,  "u8",    "u8"    },
    { AV_SAMPLE_FMT_S16, "s16be", "s16le" },
    { AV_SAMPLE_FMT_S32, "s32be", "s32le" },
    { AV_SAMPLE_FMT_FLT, "f32be", "f32le" },
    { AV_SAMPLE_FMT_DBL, "f64be", "f64le" },
};


/* AVPacket flush_pkt; */

// Function declarations
void display_picture(RendererCtx *rctx, VideoPicture *videoPicture);
void decoder_thread(void *arg);
void presenter_thread(void *arg);
void blank_window(RendererCtx *rctx);
int read_cmd(RendererCtx *rctx, int mode);


// Implementation
void
reset_rctx(RendererCtx *rctx)
{
	rctx->url = nil;
	rctx->fileservername = nil;
	rctx->filename = nil;
	rctx->isfile = 0;
	rctx->isaddr = 0;
	rctx->fileserverfd = -1;
	rctx->fileserverfid = nil;
	rctx->fileserver = nil;
	rctx->io_ctx = nil;
	rctx->format_ctx = nil;
	rctx->current_codec_ctx = nil;
	/* rctx->av_sync_type = DEFAULT_AV_SYNC_TYPE; */
	/* rctx->renderer_state = RSTATE_STOP; */
	/* rctx->next_renderer_state = RSTATE_STOP; */
	rctx->renderer_state = STOP;
	rctx->next_renderer_state = STOP;
	rctx->quit = 0;
	rctx->cmdq = nil;
	rctx->screen_width = 0;
	rctx->screen_height = 0;
	rctx->window_width = 0;
	rctx->window_height = 0;
	rctx->server_tid = 0;
	rctx->decoder_tid = 0;
	rctx->presenter_tid = 0;
	// Audio stream
	rctx->audio_stream = -1;
	rctx->audio_ctx = nil;
	rctx->audioq = nil;
	rctx->audio_buf_size = 0;
	rctx->audio_buf_index = 0;
	rctx->audio_idx = 0;
	rctx->audio_out_channels = 2;
	rctx->current_video_time = 0.0;
	rctx->previous_video_time = 0.0;
	rctx->audio_start_rt = 0;
	rctx->audio_timebase.num = 0;
	rctx->audio_timebase.den = 0;
	rctx->audio_tbd = 0.0;
	rctx->audio_vol = 100;
	rctx->swr_ctx = nil;
	// Video stream.
	rctx->sdl_window = nil;
	rctx->sdl_texture = nil;
	rctx->sdl_renderer = nil;
	rctx->video_stream = -1;
	rctx->frame_rate = 0.0;
	rctx->frame_duration = 0.0;
	rctx->video_ctx = nil;
	rctx->pictq = nil;
	rctx->yuv_ctx = nil;
	rctx->rgb_ctx = nil;
	rctx->video_idx = 0;
	rctx->frame_rgb = nil;
	rctx->rgbbuffer = nil;
	rctx->yuvbuffer = nil;
	rctx->w = 0;
	rctx->h = 0;
	rctx->aw = 0;
	rctx->ah = 0;
	rctx->video_timebase.num = 0;
	rctx->video_timebase.den = 0;
	rctx->video_tbd = 0.0;
	// Seeking
	rctx->seek_req = 0;
	rctx->seek_flags = 0;
	rctx->seek_pos = 0;
	/* rctx->frame_fmt = FRAME_FMT_RGB; */
	rctx->frame_fmt = FRAME_FMT_YUV;
	rctx->audio_devid = -1;
	rctx->audio_only = 0;
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


int
demuxerPacketRead(void *fid, uint8_t *buf, int count)
{
	LOG("demuxer reading %d bytes from fid: %p into buf: %p ...", count, fid, buf);
	CFid *cfid = (CFid*)fid;
	int ret = fsread(cfid, buf, count);
	LOG("demuxer read %d bytes", ret);
	return ret;
}


int64_t
demuxerPacketSeek(void *fid, int64_t offset, int whence)
{
	LOG("demuxer seeking fid: %p offset: %ld", fid, offset);
	CFid *cfid = (CFid*)fid;
	int64_t ret = fsseek(cfid, offset, whence);
	LOG("demuxer seek found offset %ld", ret);
	return ret;
}


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
seturl(RendererCtx *rctx, char *url)
{
	char *s, *f;
	int ret = parseurl(rctx->url, &s, &f, &rctx->isaddr, &rctx->isfile);
	if (rctx->isfile) {
		LOG("input is file, setting url to %s", url);
		setstr(&rctx->filename, url, 0);
		return;
	}
	if (ret == -1) {
		LOG("failed to parse url %s", url);
		rctx->fileservername = nil;
		rctx->filename = nil;
		return;
	}
	setstr(&rctx->fileservername, s, 0);
	setstr(&rctx->filename, f, 0);
}


int
clientdial(RendererCtx *rctx)
{
	LOG("dialing address: %s ...", rctx->fileservername);
	if ((rctx->fileserverfd = dial(rctx->fileservername, nil, nil, nil)) < 0)
		return -1;
		/* sysfatal("dial: %r"); */
	LOG("mounting address ...");
	if ((rctx->fileserver = fsmount(rctx->fileserverfd, nil)) == nil)
		return -1;
		/* sysfatal("fsmount: %r"); */
	return 0;
}


int
clientmount(RendererCtx *rctx)
{
	if ((rctx->fileserver = nsmount(rctx->fileservername, nil)) == nil)
		return -1;
		/* sysfatal("nsmount: %r"); */
	return 0;
}


void
reset_filectx(RendererCtx *rctx)
{
	LOG("deleting fileserver name ...");
	if (rctx->filename) {
		free(rctx->filename);
		rctx->filename = nil;
	}
	LOG("unmounting fileserver ...");
	if (rctx->fileserver) {
		fsunmount(rctx->fileserver);
		rctx->fileserver = nil;
	}
	LOG("closing the network connection ...");
	if (rctx->fileserverfid) {
		fsclose(rctx->fileserverfid);
		rctx->fileserverfid = nil;
	}
	LOG("closing server file descriptor ...");
	if (rctx->fileserverfd != -1) {
		close(rctx->fileserverfd);
		rctx->fileserverfd = -1;
	}
	LOG("server closed");
}


void
srvopen(Req *r)
{
	LOG("server open");
	respond(r, nil);
}


void
srvread(Req *r)
{
	LOG("server read");
	r->ofcall.count = 6;
	r->ofcall.data = "hello\n";
	respond(r, nil);
}


void
srvwrite(Req *r)
{
	LOG("server write");
	Command command;
	char cmdstr[MAX_CMD_STR_LEN];
	snprint(cmdstr, r->ifcall.count, "%s", r->ifcall.data);
	int cmdlen = r->ifcall.count;
	int arglen = 0;
	char* argstr = strchr(cmdstr, ' ');
	if (argstr != nil) {
		cmdlen = argstr - cmdstr;
		arglen = r->ifcall.count - cmdlen + 1;
		*argstr = '\0';
		argstr++;
		LOG("server cmd: %s, arg: %s", cmdstr, argstr);
		command.narg = arglen;
		command.arg = malloc(arglen);
		memcpy(command.arg, argstr, arglen);
	}
	else {
		LOG("server cmd: %s", cmdstr);
	}
	command.cmd = CMD_NONE;
	if (strncmp(cmdstr, "set", 3) == 0) {
		command.cmd = CMD_SET;
	}
	else if (strncmp(cmdstr, "vol", 3) == 0) {
		command.cmd = CMD_VOL;
	}
	else if (strncmp(cmdstr, "stop", 4) == 0) {
		command.cmd = CMD_STOP;
	}
	else if (strncmp(cmdstr, "play", 4) == 0) {
		command.cmd = CMD_PLAY;
	}
	else if (strncmp(cmdstr, "pause", 5) == 0) {
		command.cmd = CMD_PAUSE;
	}
	else if (strncmp(cmdstr, "seek", 4) == 0) {
		command.cmd = CMD_SEEK;
	}
	else if (strncmp(cmdstr, "quit", 4) == 0) {
		command.cmd = CMD_QUIT;
	}
	RendererCtx *rctx = r->fid->file->aux;
	if (rctx) {
		LOG("sending command: %d ...", command.cmd);
		send(rctx->cmdq, &command);
	}
	else {
		LOG("server file has no renderer context");
	}
	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}


int
open_9pconnection(RendererCtx *rctx)
{
	// FIXME restructure server open/close code
	LOG("opening 9P connection ...");
	if (rctx->isfile) {
		LOG("input is a file, nothing to do");
		return 0;
	}
	int ret;
	if (!rctx->fileserver) {
		if (rctx->isaddr) {
			/* rctx->fileserver = clientdial(rctx->fileservername); */
			ret = clientdial(rctx);
		}
		else {
			/* rctx->fileserver = clientmount(rctx->fileservername); */
			ret = clientmount(rctx);
		}
		/* rctx->fileserver = clientdial("tcp!localhost!5640"); */
		/* rctx->fileserver = clientdial("tcp!192.168.1.85!5640"); */
		if (ret == -1) {
			LOG("failed to open 9P connection");
			return ret;
		}
	}
	LOG("opening 9P file ...");
	CFid *fid = fsopen(rctx->fileserver, rctx->filename, OREAD);
	if (fid == nil) {
		rctx->renderer_state = RSTATE_STOP;
		blank_window(rctx);
		return -1;
	}
	rctx->fileserverfid = fid; 
	return 0;
}


Srv server = {
	.open  = srvopen,
	.read  = srvread,
	.write = srvwrite,
};


int
threadmaybackground(void)
{
	return 1;
}


void
start_server(RendererCtx *rctx)
{
	LOG("starting 9P server ...");
	char *srvname = "ommrenderer";
	/* char *mtpt = "/srv"; */
	char *mtpt = nil;
	server.tree = alloctree(nil, nil, DMDIR|0777, nil);
	// Workaround for the first directory entry not beeing visible (it exists and is readable/writable)
	// This might be a bug in plan9port 9Pfile + fuse
	/* createfile(server.tree->root, "dummy", nil, 0777, nil); */
	/* File *f = createfile(server.tree->root, "ctl", nil, 0777, nil); */
	createfile(server.tree->root, "ctl", nil, 0777, rctx);
	/* f->aux = rctx; */
	/* srv(&server); */
	/* postfd(srvname, server.srvfd); */
	// Workaround for fuse not unmounting the service ... ? 
	// ... the first access will fail but unmount it.
	/* if(mtpt && access(mtpt, AEXIST) < 0 && access(mtpt, AEXIST) < 0) */
		/* sysfatal("mountpoint %s does not exist", mtpt); */
	/* server.foreground = 1; */
	threadpostmountsrv(&server, srvname, mtpt, MREPL|MCREATE);
	/* threadexits(0); */
	LOG("9P server started.");
}


int
create_sdl_window(RendererCtx *rctx)
{
	SDL_DisplayMode DM;
	if (SDL_GetCurrentDisplayMode(0, &DM) != 0) {
		LOG("failed to get sdl display mode");
		return -1;
	}
	rctx->screen_width  = DM.w;
	rctx->screen_height = DM.h;
	int requested_window_width  = 800;
	int requested_window_height = 600;
	if (rctx->sdl_window == nil) {
		// create a window with the specified position, dimensions, and flags.
		rctx->sdl_window = SDL_CreateWindow(
			"OMM Renderer",
			SDL_WINDOWPOS_UNDEFINED,
			SDL_WINDOWPOS_UNDEFINED,
			requested_window_width,
			requested_window_height,
			/* SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI */
			SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI
			);
		SDL_GL_SetSwapInterval(1);
	}
	if (rctx->sdl_window == nil) {
		LOG("SDL: could not create window");
		return -1;
	}
	if (rctx->sdl_renderer == nil) {
		// create a 2D rendering context for the SDL_Window
		rctx->sdl_renderer = SDL_CreateRenderer(
			rctx->sdl_window,
			-1,
			SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
	}
	return 0;
}


void
get_sdl_window_size(RendererCtx *rctx)
{
	SDL_GetWindowSize(rctx->sdl_window, &rctx->w, &rctx->h);
}


int
calc_videoscale(RendererCtx *rctx)
{
	if (rctx->video_ctx == nil) {
		return -1;
	}
	int w = rctx->w;
	int h = rctx->h;
	float war = (float)h / w;
	float far = (float)rctx->video_ctx->height / rctx->video_ctx->width;
	float fsar = av_q2d(rctx->video_ctx->sample_aspect_ratio);
	fsar = rctx->video_ctx->sample_aspect_ratio.num == 0 ? 1.0 : fsar;
	int aw = h / far * fsar;
	int ah = h;
	if (war > far) {
		aw = w;
		ah = w * far / fsar;
	}
	rctx->aw = aw;
	rctx->ah = ah;
	LOG("scaling frame: %dx%d to win size: %dx%d, aspect ratio win: %f, aspect ratio frame: %f, sample aspect ratio: %f, final picture size: %dx%d",
		rctx->video_ctx->width, rctx->video_ctx->height, w, h, war, far, fsar, aw, ah);
	return 0;
}


int
resize_video(RendererCtx *rctx)
{
	if (rctx->video_ctx == nil) {
		LOG("cannot resize video picture, no video context");
		return -1;
	}
	get_sdl_window_size(rctx);
	calc_videoscale(rctx);
	rctx->blit_copy_rect.x = 0.5 * (rctx->w - rctx->aw);
	rctx->blit_copy_rect.y = 0.5 * (rctx->h - rctx->ah);
	rctx->blit_copy_rect.w = rctx->aw;
	rctx->blit_copy_rect.h = rctx->ah;
	LOG("setting scaling context and texture for video frame to size: %dx%d", rctx->aw, rctx->ah);
	if (rctx->yuv_ctx != nil) {
		av_free(rctx->yuv_ctx);
	}
	rctx->yuv_ctx = sws_getContext(
		rctx->video_ctx->width,
		rctx->video_ctx->height,
		rctx->video_ctx->pix_fmt,
		// set video size for the ffmpeg image scaler
		rctx->aw,
		rctx->ah,
		AV_PIX_FMT_YUV420P,
		SWS_BILINEAR,
		nil,
		nil,
		nil
	);
	if (rctx->sdl_texture != nil) {
		SDL_DestroyTexture(rctx->sdl_texture);
	}
	rctx->sdl_texture = SDL_CreateTexture(
		rctx->sdl_renderer,
		SDL_PIXELFORMAT_YV12,
		/* SDL_TEXTUREACCESS_STREAMING, */
		SDL_TEXTUREACCESS_TARGET,  // fast update w/o locking, can be used as a render target
		// set video size as the dimensions of the texture
		rctx->aw,
		rctx->ah
		);
	if (rctx->rgb_ctx != nil) {
		av_free(rctx->rgb_ctx);
	}
	rctx->rgb_ctx = sws_getContext(
		rctx->video_ctx->width,
		rctx->video_ctx->height,
		rctx->video_ctx->pix_fmt,
		rctx->video_ctx->width,
		rctx->video_ctx->height,
		AV_PIX_FMT_RGB24,
		SWS_BILINEAR,
		nil,
		nil,
		nil
	);
	return 0;
}


void
blank_window(RendererCtx *rctx)
{
	SDL_SetRenderDrawColor(rctx->sdl_renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
	SDL_RenderClear(rctx->sdl_renderer);
	SDL_RenderPresent(rctx->sdl_renderer);
}


int
setup_format_ctx(RendererCtx *rctx)
{
	LOG("setting up IO context ...");
	if (rctx->isfile) {
		LOG("input is a file, nothing to setup");
		return 0;
	}
	unsigned char *avctxBuffer;
	avctxBuffer = malloc(avctxBufferSize);
	AVIOContext *io_ctx = avio_alloc_context(
		avctxBuffer,          // buffer
		avctxBufferSize,      // buffer size
		0,                    // buffer is only readable - set to 1 for read/write
		rctx->fileserverfid,  // user specified data
		demuxerPacketRead,    // function for reading packets
		nil,                  // function for writing packets
		demuxerPacketSeek     // function for seeking to position in stream
		);
	if(io_ctx == nil) {
		LOG("failed to allocate memory for ffmpeg av io context");
		return -1;
	}
	AVFormatContext *format_ctx = avformat_alloc_context();
	if (format_ctx == nil) {
	  LOG("failed to allocate av format context");
	  return -1;
	}
	format_ctx->pb = io_ctx;
	rctx->io_ctx = io_ctx;
	rctx->format_ctx = format_ctx;
	return 0;
}


int
open_input_stream(RendererCtx *rctx)
{
	LOG("opening input stream ...");
	int ret;
	if (rctx->isfile) {
		ret = avformat_open_input(&rctx->format_ctx, rctx->filename, nil, nil);
	} else {
		ret = avformat_open_input(&rctx->format_ctx, nil, nil, nil);
	}
	if (ret < 0) {
		LOG("could not open file %s", rctx->filename);
		if (rctx->io_ctx) {
			avio_context_free(&rctx->io_ctx);
		}
		avformat_close_input(&rctx->format_ctx);
		if (rctx->format_ctx) {
			avformat_free_context(rctx->format_ctx);
		}
		return -1;
	}
	LOG("opened input stream");
	return 0;
}


int
open_codec_context(RendererCtx *rctx, enum AVMediaType type, int *stream_idx, AVCodecContext **dec_ctx)
{
    int ret, stream_index;
    AVStream *st;
    const AVCodec *dec = NULL;

    ret = av_find_best_stream(rctx->format_ctx, type, -1, -1, NULL, 0);
    if (ret < 0) {
        LOG("could not find %s stream", av_get_media_type_string(type));
        return ret;
    } else {
        stream_index = ret;
        st = rctx->format_ctx->streams[stream_index];
        // find decoder for the stream
        dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) {
            LOG("failed to find %s codec", av_get_media_type_string(type));
            return AVERROR(EINVAL);
        }
        // Allocate a codec context for the decoder
        *dec_ctx = avcodec_alloc_context3(dec);
        if (!*dec_ctx) {
            LOG("failed to allocate the %s codec context", av_get_media_type_string(type));
            return AVERROR(ENOMEM);
        }
        // Copy codec parameters from input stream to output codec context
        if ((ret = avcodec_parameters_to_context(*dec_ctx, st->codecpar)) < 0) {
            LOG("failed to copy %s codec parameters to decoder context",
                    av_get_media_type_string(type));
            return ret;
        }
        // Init the decoders
        if ((ret = avcodec_open2(*dec_ctx, dec, NULL)) < 0) {
            fprintf(stderr, "Failed to open %s codec\n",
                    av_get_media_type_string(type));
            return ret;
        }
        *stream_idx = stream_index;
    }
    return 0;
}


int
open_stream_component(RendererCtx *rctx, int stream_index)
{
	LOG("opening stream component ...");
	AVFormatContext *format_ctx = rctx->format_ctx;
	if (stream_index < 0 || stream_index >= format_ctx->nb_streams) {
		LOG("invalid stream index");
		return -1;
	}
	AVCodec *codec = avcodec_find_decoder(format_ctx->streams[stream_index]->codecpar->codec_id);
	if (codec == nil) {
		LOG("unsupported codec");
		return -1;
	}
	AVCodecContext *codecCtx = avcodec_alloc_context3(codec);
	int ret = avcodec_parameters_to_context(codecCtx, format_ctx->streams[stream_index]->codecpar);
	if (ret != 0) {
		LOG("could not copy codec context");
		return -1;
	}
	if (avcodec_open2(codecCtx, codec, nil) < 0) {
		LOG("could not open codec");
		return -1;
	}
	switch (codecCtx->codec_type) {
		case AVMEDIA_TYPE_AUDIO:
		{
			LOG("setting up audio stream context ...");
			rctx->audio_stream = stream_index;
			rctx->audio_ctx = codecCtx;
			rctx->audio_buf_size = 0;
			rctx->audio_buf_index = 0;
			memset(&rctx->audio_pkt, 0, sizeof(rctx->audio_pkt));
			rctx->audioq = chancreate(sizeof(AVPacket), MAX_AUDIOQ_SIZE);
			rctx->presenter_tid = threadcreate(presenter_thread, rctx, THREAD_STACK_SIZE);
			rctx->audio_timebase = rctx->format_ctx->streams[stream_index]->time_base;
			rctx->audio_tbd = av_q2d(rctx->audio_timebase);
			LOG("timebase of audio stream: %d/%d = %f",
				rctx->audio_timebase.num, rctx->audio_timebase.den, rctx->audio_tbd);
			LOG("presenter thread created with id: %i", rctx->presenter_tid);
			LOG("setting up audio device with requested specs - sample_rate: %d, channels: %d ...",
				codecCtx->sample_rate, rctx->audio_out_channels);
			SDL_AudioSpec wanted_specs;
			wanted_specs.freq = codecCtx->sample_rate;
			wanted_specs.format = AUDIO_S16SYS;
			wanted_specs.channels = rctx->audio_out_channels;
			wanted_specs.silence = 0;
			wanted_specs.samples = SDL_AUDIO_BUFFER_SIZE;
			wanted_specs.callback = nil;
			wanted_specs.userdata = rctx;
			rctx->audio_devid = SDL_OpenAudioDevice(nil, 0, &wanted_specs, &rctx->specs, 0);
			if (rctx->audio_devid == 0) {
				LOG("SDL_OpenAudio: %s", SDL_GetError());
				return -1;
			}
			LOG("audio device with id: %d opened successfully", rctx->audio_devid);
			LOG("audio specs are sample rate: %d, channels: %d, channel layout: 0x%lx, sample fmt: 0x%x", rctx->specs.freq, rctx->specs.channels, codecCtx->channel_layout, codecCtx->sample_fmt);
			rctx->swr_ctx = swr_alloc_set_opts(NULL,  // we're allocating a new context
				AV_CH_LAYOUT_STEREO,      // out_ch_layout
				AV_SAMPLE_FMT_S16,        // out_sample_fmt
				codecCtx->sample_rate,    // out_sample_rate
				codecCtx->channel_layout, // in_ch_layout
				codecCtx->sample_fmt,     // in_sample_fmt
				codecCtx->sample_rate,    // in_sample_rate
				0,                        // log_offset
				NULL);                    // log_ctx
			if (rctx->swr_ctx == nil) {
				LOG("failed to alloc audio resampling context");
				return -1;
			}
			int ret = swr_init(rctx->swr_ctx);
			if (ret < 0) {
				LOG("failed to init audio resampling context %s", av_err2str(ret));
				return -1;
			}
			LOG("starting sdl audio processing ...");
			SDL_PauseAudioDevice(rctx->audio_devid, 0);
		}
		break;
		case AVMEDIA_TYPE_VIDEO:
		{
			LOG("setting up video stream context ...");
			rctx->video_stream = stream_index;
			rctx->video_ctx = codecCtx;
			rctx->pictq = chancreate(sizeof(VideoPicture), VIDEO_PICTURE_QUEUE_SIZE);
			resize_video(rctx);
			rctx->video_timebase = rctx->format_ctx->streams[stream_index]->time_base;
			rctx->video_tbd = av_q2d(rctx->video_timebase);
			LOG("timebase of video stream: %d/%d = %f",
				rctx->video_timebase.num, rctx->video_timebase.den, rctx->video_tbd);
			LOG("sample aspect ratio: %d/%d",
				codecCtx->sample_aspect_ratio.num,
				codecCtx->sample_aspect_ratio.den);
		}
		break;
		default:
		{
			LOG("stream contains unhandled codec type");
		}
		break;
	}
	return 0;
}


int
open_stream_components(RendererCtx *rctx)
{
	int ret = avformat_find_stream_info(rctx->format_ctx, nil);
	if (ret < 0) {
		LOG("Could not find stream information: %s.", rctx->filename);
		return -1;
	}
	if (_DEBUG_) {
		av_dump_format(rctx->format_ctx, 0, rctx->filename, 0);
	}
	rctx->video_stream = -1;
	rctx->audio_stream = -1;
	for (int i = 0; i < rctx->format_ctx->nb_streams; i++)
	{
		if (rctx->format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && rctx->video_stream < 0) {
			rctx->video_stream = i;
			LOG("selecting stream %d for video", rctx->video_stream);
		}
		if (rctx->format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && rctx->audio_stream < 0) {
			rctx->audio_stream = i;
			LOG("selecting stream %d for audio", rctx->audio_stream);
		}
	}
	if (rctx->video_stream == -1) {
		LOG("Could not find video stream.");
	}
	else {
		ret = open_stream_component(rctx, rctx->video_stream);
		if (ret < 0) {
			printf("Could not open video codec.\n");
			return -1;
		}
		LOG("video stream component opened successfully.");
	}
	if (rctx->audio_stream == -1) {
		LOG("Could not find audio stream.");
	}
	else {
		ret = open_stream_component(rctx, rctx->audio_stream);
		// check audio codec was opened correctly
		if (ret < 0) {
			LOG("Could not open audio codec.");
			return -1;
		}
		LOG("audio stream component opened successfully.");
	}
	if (rctx->video_stream < 0 && rctx->audio_stream < 0) {
		LOG("both video and audio stream missing");
		return -1;
	}
	return 0;
}


int
alloc_buffers(RendererCtx *rctx)
{
	if (rctx->video_ctx) {
		// yuv buffer for displaying to screen
	    int yuv_num_bytes = av_image_get_buffer_size(
			AV_PIX_FMT_YUV420P,
			rctx->w,
			rctx->h,
			/* rctx->aw, */
			/* rctx->ah, */
			// crash on bunny with buffer size below
			/* rctx->video_ctx->width, */
			/* rctx->video_ctx->height, */
			32
			);
		rctx->yuvbuffer = (uint8_t *) av_malloc(yuv_num_bytes * sizeof(uint8_t));
		// rgb buffer for saving to disc
	    rctx->frame_rgb = av_frame_alloc();
	    int rgb_num_bytes;
	    rgb_num_bytes = av_image_get_buffer_size(
			AV_PIX_FMT_RGB24,
			rctx->video_ctx->width,
			rctx->video_ctx->height,
			32
			);
	    rctx->rgbbuffer = (uint8_t *) av_malloc(rgb_num_bytes * sizeof(uint8_t));
	    av_image_fill_arrays(
			rctx->frame_rgb->data,
			rctx->frame_rgb->linesize,
			rctx->rgbbuffer,
			AV_PIX_FMT_RGB24,
			rctx->video_ctx->width,
			rctx->video_ctx->height,
			32
	    );
	}
	return 0;
}


int
read_packet(RendererCtx *rctx, AVPacket *packet)
{
	int demuxer_ret = av_read_frame(rctx->format_ctx, packet);
	if (demuxer_ret < 0) {
		LOG("failed to read av packet: %s", av_err2str(demuxer_ret));
		if (demuxer_ret == AVERROR_EOF) {
			LOG("EOF");
		}
		rctx->renderer_state = RSTATE_STOP;
		reset_filectx(rctx);
		blank_window(rctx);
		return -1;
	}
	if (packet->size == 0) {
		LOG("packet size is zero, exiting demuxer thread");
		return -1;
	}
	char *stream = "not selected";
	if (packet->stream_index == rctx->audio_stream) {
		stream = "audio";
	}
	else if (packet->stream_index == rctx->video_stream) {
		stream = "video";
	}
	LOG("read %s packet with size: %d, pts: %ld, dts: %ld, duration: %ld, pos: %ld",
		stream, packet->size, packet->pts, packet->dts, packet->duration, packet->pos);
	double tbdms = 1000 * ((packet->stream_index == rctx->audio_stream) ?
		rctx->audio_tbd : rctx->video_tbd);
	LOG("%s packet times pts: %.2fms, dts: %.2fms, duration: %.2fms",
		 stream, tbdms * packet->pts, tbdms * packet->dts, tbdms * packet->duration);
	return 0;
}


int
write_packet_to_decoder(RendererCtx *rctx, AVPacket* packet)
{
	AVCodecContext *codecCtx = nil;
	if (packet->stream_index == rctx->video_stream) {
		LOG("sending video packet of size %d to decoder", packet->size);
		codecCtx = rctx->video_ctx;
	}
	else if (packet->stream_index == rctx->audio_stream) {
		LOG("sending audio packet of size %d to decoder", packet->size);
		codecCtx = rctx->audio_ctx;
	}
	else {
		LOG("skipping packet of size %d, not a selected AV packet", packet->size);
		av_packet_unref(packet);
		return -1;
	}
	int decsend_ret = avcodec_send_packet(codecCtx, packet);
	LOG("sending packet of size %d to decoder returned: %d", packet->size, decsend_ret);
	if (decsend_ret == AVERROR(EAGAIN)) {
		LOG("AVERROR = EAGAIN: input not accepted, receive frame from decoder first");
	}
	if (decsend_ret == AVERROR(EINVAL)) {
		LOG("AVERROR = EINVAL: codec not opened or requires flush");
	}
	if (decsend_ret == AVERROR(ENOMEM)) {
		LOG("AVERROR = ENOMEM: failed to queue packet");
	}
	if (decsend_ret == AVERROR_EOF) {
		LOG("AVERROR = EOF: decoder has been flushed");
		rctx->renderer_state = RSTATE_STOP;
		reset_filectx(rctx);
		blank_window(rctx);
	}
	if (decsend_ret < 0) {
		LOG("error sending packet to decoder: %s", av_err2str(decsend_ret));
		return -1;
	}
	rctx->current_codec_ctx = codecCtx;
	return 0;
}


int
read_frame_from_decoder(RendererCtx *rctx, AVFrame *frame)
{
	LOG("reading decoded frame from decoder ...");
	int ret = avcodec_receive_frame(rctx->current_codec_ctx, frame);
	// check if entire frame was decoded
	if (ret == AVERROR(EAGAIN)) {
		LOG("no more decoded frames to squeeze out of current av packet");
		return 2;
	}
	if (ret == AVERROR_EOF) {
		LOG("end of file: AVERROR = EOF");
		return -1;
	}
	if (ret == AVERROR(EINVAL)) {
		LOG("decoding error: AVERROR = EINVAL");
		return -1;
	}
	if (ret < 0) {
		LOG("error reading decoded frame from decoder: %s", av_err2str(ret));
		return -1;
	}
	LOG("received decoded frame");
	return 0;
}


int
create_pristine_picture_from_frame(RendererCtx *rctx, AVFrame *frame, VideoPicture *videoPicture)
{
	// FIXME sending pristine frames over the video picture channel doesn't work
	videoPicture->pix_fmt = rctx->current_codec_ctx->pix_fmt;
	LOG("AV_NUM_DATA_POINTERS: %d", AV_NUM_DATA_POINTERS);
	/* memcpy(videoPicture->linesizes, frame->linesize, 4 * sizeof(uint8_t*) / 8); */
	/* memcpy(videoPicture->linesizes, frame->linesize, AV_NUM_DATA_POINTERS); */
	/* memcpy(videoPicture->linesizes, frame->linesize, AV_NUM_DATA_POINTERS * sizeof(uint8_t*)); */
	/* memcpy(videoPicture->linesizes, frame->linesize, AV_NUM_DATA_POINTERS * sizeof(uint8_t*) / 8); */
	/* memcpy(videoPicture->planes, frame->data, AV_NUM_DATA_POINTERS * sizeof(uint8_t*)); */
	/* memcpy(videoPicture->planes, frame->data, 4 * sizeof(uint8_t*) / 8); */
	// FIXME avoid hardcoding parameter align to 32 ...
	LOG("allocating video picture for queueing ...");
	int frame_size = av_image_alloc(
		videoPicture->planes,
		frame->linesize,
		rctx->current_codec_ctx->width,
		rctx->current_codec_ctx->height,
		rctx->current_codec_ctx->pix_fmt,
		32
	);
	USED(frame_size);
	LOG("copying video picture for queueing ...");
	av_image_copy(
		videoPicture->planes,
		//videoPicture->linesizes,
	          frame->linesize,
	          (uint8_t const**)frame->data,
	          frame->linesize,
		rctx->current_codec_ctx->pix_fmt,
		rctx->current_codec_ctx->width,
		rctx->current_codec_ctx->height
	);
	/* av_frame_copy(); */
	return 0;
}


int
create_rgb_picture_from_frame(RendererCtx *rctx, AVFrame *frame, VideoPicture *videoPicture)
{
	sws_scale(
	    rctx->rgb_ctx,
	    (uint8_t const * const *)frame->data,
	    frame->linesize,
	    0,
	    rctx->current_codec_ctx->height,
	    rctx->frame_rgb->data,
	    rctx->frame_rgb->linesize
	);
	// av_frame_unref(frame);
	videoPicture->linesize = rctx->frame_rgb->linesize[0];
	int rgb_num_bytes = av_image_get_buffer_size(
		AV_PIX_FMT_RGB24,
		rctx->current_codec_ctx->width,
		rctx->current_codec_ctx->height,
		32
		);
	videoPicture->rgbbuf = (uint8_t *) av_malloc(rgb_num_bytes * sizeof(uint8_t));
	memcpy(videoPicture->rgbbuf, rctx->frame_rgb->data[0], rgb_num_bytes);
	return 0;
}


int
create_yuv_picture_from_frame(RendererCtx *rctx, AVFrame *frame, VideoPicture *videoPicture)
{
	LOG("scaling video picture (height %d) to target size %dx%d before queueing",
		rctx->current_codec_ctx->height, rctx->aw, rctx->ah);
	videoPicture->frame = av_frame_alloc();
	av_image_fill_arrays(
			videoPicture->frame->data,
			videoPicture->frame->linesize,
			rctx->yuvbuffer,
			AV_PIX_FMT_YUV420P,
			// set video size of picture to send to queue here
			rctx->aw, 
			rctx->ah,
			32
	);
	sws_scale(
	    rctx->yuv_ctx,
	    (uint8_t const * const *)frame->data,
	    frame->linesize,
	    0,
	    // set video height here to select the slice in the *SOURCE* picture to scale (usually the whole picture)
	    rctx->current_codec_ctx->height,
	    videoPicture->frame->data,
	    videoPicture->frame->linesize
	);
	LOG("video picture created.");
	return 0;
}


int
create_sample_from_frame(RendererCtx *rctx, AVFrame *frame, AudioSample *audioSample)
{
	int bytes_per_sample = 2 * rctx->audio_out_channels;
	int bytes_per_sec = rctx->current_codec_ctx->sample_rate * bytes_per_sample;
	audioSample->sample = malloc(MAX_AUDIO_FRAME_SIZE);
	int nbsamples = swr_convert(
			rctx->swr_ctx,
			&audioSample->sample,
			MAX_AUDIO_FRAME_SIZE / bytes_per_sample,
			(const uint8_t**) frame->data,
			frame->nb_samples
	);
	if (nbsamples < 0) {
		LOG("resampling audio failed");
		return 0;
	}
	int data_size = nbsamples * bytes_per_sample;
	double sample_duration = 1000.0 * data_size / bytes_per_sec;
	audioSample->size = data_size;
	audioSample->duration = sample_duration;
	LOG("resampled audio bytes: %d", data_size);
	LOG("audio sample rate: %d, channels: %d, duration: %.2fms",
		rctx->current_codec_ctx->sample_rate, rctx->audio_out_channels, audioSample->duration);
	return nbsamples;
}


void
send_picture_to_queue(RendererCtx *rctx, VideoPicture *videoPicture)
{
	LOG("==> sending picture with idx: %d, pts: %.2fms to picture queue ...", videoPicture->idx, videoPicture->pts);
	int sendret = send(rctx->pictq, videoPicture);
	if (sendret == 1) {
		LOG("==> sending picture with idx: %d, pts: %.2fms to picture queue succeeded.", videoPicture->idx, videoPicture->pts);
	}
	else if (sendret == -1) {
		LOG("==> sending picture to picture queue interrupted");
	}
	else {
		LOG("==> unforseen error when sending picture to picture queue");
	}
}


void
send_sample_to_queue(RendererCtx *rctx, AudioSample *audioSample)
{
	int sendret = send(rctx->audioq, audioSample);
	if (sendret == 1) {
		LOG("==> sending audio sample with idx: %d, pts: %.2fms to audio queue succeeded.", audioSample->idx, audioSample->pts);
		/* LOG("==> sending audio sample to audio queue succeeded."); */
	}
	else if (sendret == -1) {
		LOG("==> sending audio sample to audio queue interrupted");
	}
	else {
		LOG("==> unforseen error when sending audio sample to audio queue");
	}
}


void state_stop(RendererCtx* rctx)
{
	while (read_cmd(rctx, READCMD_BLOCK) == KEEP_STATE) {
	}
}


void state_run(RendererCtx* rctx)
{
	AVPacket *packet = av_packet_alloc();
	if (packet == nil) {
		LOG("Could not allocate AVPacket.");
		rctx->renderer_state = transitions[CMD_ERR][rctx->renderer_state];
	}
	AVFrame *frame = av_frame_alloc();
	if (frame == nil) {
		printf("Could not allocate AVFrame.\n");
		rctx->renderer_state = transitions[CMD_ERR][rctx->renderer_state];
	}
	double audio_pts = 0.0;
	double video_pts = 0.0;

	// Main decoder loop
	for (;;) {
		if (read_cmd(rctx, READCMD_POLL) == CHANGE_STATE) {
			return;
		}
		if (read_packet(rctx, packet) == -1) {
			rctx->renderer_state = transitions[CMD_ERR][rctx->renderer_state];
		}
		if (write_packet_to_decoder(rctx, packet) == -1) {
			rctx->renderer_state = transitions[CMD_ERR][rctx->renderer_state];
		}
		// This loop is only needed when we get more than one decoded frame out
		// of one packet read from the demuxer
		int decoder_ret = 0;
		while (decoder_ret == 0) {
			decoder_ret = read_frame_from_decoder(rctx, frame);
			if (decoder_ret == -1) {
				rctx->renderer_state = transitions[CMD_ERR][rctx->renderer_state];
			}
			if (decoder_ret == 2) {
				break;
			}
			// TODO it would be nicer to check for the frame type instead for the codec context
			if (rctx->current_codec_ctx == rctx->video_ctx) {
				rctx->video_idx++;
				rctx->frame_rate = av_q2d(rctx->video_ctx->framerate);
				rctx->frame_duration = 1000.0 / rctx->frame_rate;
				LOG("video frame duration: %.2fms, fps: %.2f",
					rctx->frame_duration, 1000.0 / rctx->frame_duration);
				video_pts += rctx->frame_duration;
				VideoPicture videoPicture = {
					.frame = nil,
					.rgbbuf = nil,
					.planes = nil,
					.width = rctx->aw,
					.height = rctx->ah,
					.idx = rctx->video_idx,
					.pts = video_pts,
					};
				if (rctx->frame_fmt == FRAME_FMT_PRISTINE) {
					if (create_pristine_picture_from_frame(rctx, frame, &videoPicture) == 2) {
						break;
					}
				}
				else if (rctx->frame_fmt == FRAME_FMT_RGB) {
					if (create_rgb_picture_from_frame(rctx, frame, &videoPicture) == 2) {
						break;
					}
			    }
				else if (rctx->frame_fmt == FRAME_FMT_YUV) {
					if (create_yuv_picture_from_frame(rctx, frame, &videoPicture) == 2) {
						break;
					}
			    }
				if (!rctx->audio_only) {
					send_picture_to_queue(rctx, &videoPicture);
				}
			}
			else if (rctx->current_codec_ctx == rctx->audio_ctx) {

				// write audio sample to file after decoding, before resampling
				/* LOG("writing pcm sample to file, channels: %d, sample size: %d, sample count: %d", */
					/* rctx->current_codec_ctx->channels, */
					/* rctx->current_codec_ctx->bits_per_raw_sample, */
					/* //rctx->current_codec_ctx->frame_size, */
					/* frame->nb_samples */
					/* ); */
				/* LOG("writing pcm sample of size %d to file", frame->linesize[0]); */
				/* fwrite(frame->data, */
					/* //rctx->current_codec_ctx->channels * rctx->current_codec_ctx->bits_per_raw_sample / 8, */
					/* //rctx->current_codec_ctx->frame_size, */
					/* 1, */
					/* frame->linesize[0], */
					/* //frame->nb_samples, */
					/* audio_out); */

				rctx->audio_idx++;
				AudioSample audioSample = {
					.idx = rctx->audio_idx,
					/* .sample = malloc(sizeof(rctx->audio_buf)), */
					};
				if (create_sample_from_frame(rctx, frame, &audioSample) == 0) {
					break;
				}
				audio_pts += audioSample.duration;
				audioSample.pts = audio_pts;
				send_sample_to_queue(rctx, &audioSample);

				// write audio sample to file after decoding, after resampling, before sending to queue
				//fwrite(audioSample.sample, 1, audioSample.size, audio_out);
			}
			else {
				LOG("non AV packet from demuxer, ignoring");
			}
		}
		av_packet_unref(packet);
		av_frame_unref(frame);
	}
}


void state_idle(RendererCtx* rctx)
{
	while (read_cmd(rctx, READCMD_BLOCK) == KEEP_STATE) {
	}
}


void state_exit(RendererCtx* rctx)
{
	rctx->quit = 1;
}


void state_load(RendererCtx* rctx)
{
	if (open_9pconnection(rctx) == -1) {
		rctx->renderer_state = transitions[CMD_ERR][rctx->renderer_state];
		return;
	}
	if (setup_format_ctx(rctx) == -1) {
		rctx->renderer_state = transitions[CMD_ERR][rctx->renderer_state];
		return;
	}
	if (open_input_stream(rctx) == -1) {
		rctx->renderer_state = transitions[CMD_ERR][rctx->renderer_state];
		return;
	}
	if (open_stream_components(rctx) == -1) {
		rctx->renderer_state = transitions[CMD_ERR][rctx->renderer_state];
		return;
	}
	if (alloc_buffers(rctx) == -1) {
		rctx->renderer_state = transitions[CMD_ERR][rctx->renderer_state];
		return;
	}
	rctx->renderer_state = transitions[CMD_NONE][rctx->renderer_state];
}


void state_unload(RendererCtx* rctx)
{
}


void state_engage(RendererCtx* rctx)
{
}


void state_disengage(RendererCtx* rctx)
{
}


void
cmd_set(RendererCtx *rctx, char *arg, int narg)
{
	setstr(&rctx->url, arg, narg);
	seturl(rctx, rctx->url);
}


/* void */
/* cmd_stop(RendererCtx *rctx, char *arg, int narg) */
/* { */
	/* // Do nothing */
/* } */


/* void */
/* cmd_play(RendererCtx *rctx, char *arg, int narg) */
/* { */
	/* // Do nothing */
/* } */


/* void */
/* cmd_pause(RendererCtx *rctx, char *arg, int narg) */
/* { */
	/* // Do nothing */
/* } */


void
cmd_quit(RendererCtx *rctx, char *arg, int narg)
{
	rctx->quit = 1;
}


void
cmd_seek(RendererCtx *rctx, char *arg, int narg)
{
	// TODO implement cmd_seek()
}


void
cmd_vol(RendererCtx *rctx, char *arg, int narg)
{
	// TODO implement cmd_vol()
}


int
read_cmd(RendererCtx *rctx, int mode)
{
	int ret;
	Command cmd = {.cmd = -1, .arg = nil, .narg = 0};
	if (mode == READCMD_BLOCK) {
		ret = recv(rctx->cmdq, &cmd);
	}
	else if (mode == READCMD_POLL) {
		ret = nbrecv(rctx->cmdq, &cmd);
	}
	else {
		LOG("unsupported read command mode");
		return KEEP_STATE;
	}
	if (ret == -1) {
		LOG("receiving command interrupted");
		return KEEP_STATE;
	}
	if (ret == 1) {
		LOG("<== received command: %d", cmd.cmd);
		if (cmds[cmd.cmd] == nil) {
			LOG("command is nil, nothing to execute");
		}
		else {
			cmds[cmd.cmd](rctx, cmd.arg, cmd.narg);
		}
		// FIXME find a better check for allocated cmd arg
		if (cmd.arg != nil && cmd.narg > 0) {
			free(cmd.arg);
		}
		int next_renderer_state = transitions[cmd.cmd][rctx->renderer_state];
		LOG("state: %d -> %d", rctx->renderer_state, next_renderer_state);
		if (next_renderer_state == rctx->renderer_state) {
			return KEEP_STATE;
		}
		rctx->renderer_state = next_renderer_state;
		return CHANGE_STATE;
	}
	return KEEP_STATE;
}


/* int */
/* poll_cmd(RendererCtx *rctx, Command *cmd) */
/* { */
	/* int cmdret = nbrecv(rctx->cmdq, cmd); */
	/* if (cmdret == 1) { */
		/* LOG("<== received command: %d", cmd->cmd); */
		/* return 0; */
	/* } */
	/* return 1; */
/* } */


void
decoder_thread(void *arg)
{
	RendererCtx *rctx = (RendererCtx *)arg;
	LOG("decoder thread started with id: %d", rctx->decoder_tid);

	// Initial renderer state is LOAD only if an url was given as a command line argument
	// otherwise it defaults to STOP
	while(!rctx->quit) {
		LOG("entering state %d", rctx->renderer_state);
		states[rctx->renderer_state](rctx);
	}
}


int
read_cmd0(RendererCtx *rctx)
{
	while (rctx->filename == nil || rctx->renderer_state == RSTATE_STOP) {
		LOG("renderer stopped or no av stream file specified, waiting for command ...");
		blank_window(rctx);
		Command cmd;
		int cmdret = recv(rctx->cmdq, &cmd);
		if (cmdret == 1) {
			LOG("<== received command: %d", cmd.cmd);
			if (cmd.cmd == CMD_SET) {
				setstr(&rctx->url, cmd.arg, cmd.narg);
				seturl(rctx, rctx->url);
				free(cmd.arg);
				cmd.arg = nil;
			}
			else if (cmd.cmd == CMD_PLAY) {
				rctx->renderer_state = RSTATE_PLAY;
			}
			else if (cmd.cmd == CMD_STOP) {
				rctx->renderer_state = RSTATE_STOP;
			}
			else if (cmd.cmd == CMD_QUIT) {
				return 1;
			}
		}
		else {
			LOG("failed to receive command");
		}
	}
	return 0;
}


int
poll_cmd0(RendererCtx *rctx)
{
	if (rctx->renderer_state == RSTATE_PLAY) {
		Command cmd;
		int cmdret = nbrecv(rctx->cmdq, &cmd);
		if (cmdret == 1) {
			LOG("<== received command: %d", cmd.cmd);
			if (cmd.cmd == CMD_PAUSE) {
				rctx->renderer_state = RSTATE_PAUSE;
				cmdret = recv(rctx->cmdq, &cmd);
				while (cmdret != 1 || cmd.cmd != CMD_PAUSE) {
					LOG("<== received command: %d", cmd.cmd);
					cmdret = recv(rctx->cmdq, &cmd);
				}
				rctx->renderer_state = RSTATE_PLAY;
			}
			else if (cmd.cmd == CMD_STOP) {
				rctx->renderer_state = RSTATE_STOP;
				reset_filectx(rctx);
				blank_window(rctx);
				return -1;
			}
			else if (cmd.cmd == CMD_SEEK) {
				uint64_t seekpos = atoll(cmd.arg);
				av_seek_frame(rctx->format_ctx, rctx->audio_stream, seekpos / rctx->audio_tbd, 0);
				av_seek_frame(rctx->format_ctx, rctx->video_stream, seekpos / rctx->video_tbd, 0);
				/* rctx->renderer_state = RSTATE_SEEK; */
			}
			else if (cmd.cmd == CMD_VOL) {
				int vol = atoi(cmd.arg);
				if (vol >=0 && vol <= 100) {
					rctx->audio_vol = vol;
				}
			}
			else if (cmd.cmd == CMD_QUIT) {
				return 1;
			}
		}
	}
	return 0;
}


void
decoder_thread0(void *arg)
{
	RendererCtx *rctx = (RendererCtx *)arg;
	LOG("decoder thread started with id: %d", rctx->decoder_tid);

start:
	// FIXME handle renderer state properly in any situation
	rctx->renderer_state = RSTATE_STOP;
	if (read_cmd0(rctx) == 1) {
		goto quit;
	}

	if (open_9pconnection(rctx) == -1) {
		goto start;
	}
	if (setup_format_ctx(rctx) == -1) {
		goto start;
	}
	if (open_input_stream(rctx) == -1) {
		goto start;
	}
	if (open_stream_components(rctx) == -1) {
		goto start;
	}
	if (alloc_buffers(rctx) == -1) {
		goto start;
	}

	AVPacket *packet = av_packet_alloc();
	if (packet == nil) {
		LOG("Could not allocate AVPacket.");
		goto start;
	}
	AVFrame *frame = av_frame_alloc();
	if (frame == nil) {
		printf("Could not allocate AVFrame.\n");
		goto start;
	}
	double audio_pts = 0.0;
	double video_pts = 0.0;

	// Main decoder loop
	for (;;) {
		int jmp = poll_cmd0(rctx);
		if (jmp == -1) {
			goto start;
		}
		if (jmp == 1) {
			goto quit;
		}
		if (read_packet(rctx, packet) == -1) {
			// FIXME maybe better continue here ...?
			goto start;
		}
		if (write_packet_to_decoder(rctx, packet) == -1) {
			continue;
		}
		// This loop is only needed when we get more than one decoded frame out
		// of one packet read from the demuxer
		int decoder_ret = 0;
		while (decoder_ret == 0) {
			decoder_ret = read_frame_from_decoder(rctx, frame);
			if (decoder_ret == -1) {
				goto start;
			}
			if (decoder_ret == 2) {
				break;
			}
			// TODO it would be nicer to check for the frame type instead for the codec context
			if (rctx->current_codec_ctx == rctx->video_ctx) {
				rctx->video_idx++;
				rctx->frame_rate = av_q2d(rctx->video_ctx->framerate);
				rctx->frame_duration = 1000.0 / rctx->frame_rate;
				LOG("video frame duration: %.2fms, fps: %.2f",
					rctx->frame_duration, 1000.0 / rctx->frame_duration);
				video_pts += rctx->frame_duration;
				VideoPicture videoPicture = {
					.frame = nil,
					.rgbbuf = nil,
					.planes = nil,
					.width = rctx->aw,
					.height = rctx->ah,
					.idx = rctx->video_idx,
					.pts = video_pts,
					};
				if (rctx->frame_fmt == FRAME_FMT_PRISTINE) {
					if (create_pristine_picture_from_frame(rctx, frame, &videoPicture) == 2) {
						break;
					}
				}
				else if (rctx->frame_fmt == FRAME_FMT_RGB) {
					if (create_rgb_picture_from_frame(rctx, frame, &videoPicture) == 2) {
						break;
					}
			    }
				else if (rctx->frame_fmt == FRAME_FMT_YUV) {
					if (create_yuv_picture_from_frame(rctx, frame, &videoPicture) == 2) {
						break;
					}
			    }
				if (!rctx->audio_only) {
					send_picture_to_queue(rctx, &videoPicture);
				}
			}
			else if (rctx->current_codec_ctx == rctx->audio_ctx) {

				// write audio sample to file after decoding, before resampling
				/* LOG("writing pcm sample to file, channels: %d, sample size: %d, sample count: %d", */
					/* rctx->current_codec_ctx->channels, */
					/* rctx->current_codec_ctx->bits_per_raw_sample, */
					/* //rctx->current_codec_ctx->frame_size, */
					/* frame->nb_samples */
					/* ); */
				/* LOG("writing pcm sample of size %d to file", frame->linesize[0]); */
				/* fwrite(frame->data, */
					/* //rctx->current_codec_ctx->channels * rctx->current_codec_ctx->bits_per_raw_sample / 8, */
					/* //rctx->current_codec_ctx->frame_size, */
					/* 1, */
					/* frame->linesize[0], */
					/* //frame->nb_samples, */
					/* audio_out); */

				rctx->audio_idx++;
				AudioSample audioSample = {
					.idx = rctx->audio_idx,
					/* .sample = malloc(sizeof(rctx->audio_buf)), */
					};
				if (create_sample_from_frame(rctx, frame, &audioSample) == 0) {
					break;
				}
				audio_pts += audioSample.duration;
				audioSample.pts = audio_pts;
				send_sample_to_queue(rctx, &audioSample);

				// write audio sample to file after decoding, after resampling, before sending to queue
				//fwrite(audioSample.sample, 1, audioSample.size, audio_out);
			}
			else {
				LOG("non AV packet from demuxer, ignoring");
			}
		}
		av_packet_unref(packet);
		av_frame_unref(frame);
	}

quit:
	rctx->renderer_state = RSTATE_QUIT;
	// Clean up the decoder thread
	if (rctx->io_ctx) {
		avio_context_free(&rctx->io_ctx);
	}
	avformat_close_input(&rctx->format_ctx);
	if (rctx->format_ctx) {
		avformat_free_context(rctx->format_ctx);
	}
	/* if (rctx->videoq) { */
		/* chanfree(rctx->videoq); */
	/* } */
	if (rctx->audioq) {
		chanfree(rctx->audioq);
	}
	if (rctx->pictq) {
		chanfree(rctx->pictq);
	}
	if (rctx->swr_ctx) {
		swr_free(&rctx->swr_ctx);
	}
	fclose(audio_out);
	// in case of failure, push the FF_QUIT_EVENT and return
	LOG("quitting decoder thread");
	threadexitsall("end of file");
}


void
receive_picture(RendererCtx *rctx, VideoPicture *videoPicture)
{
	LOG("receiving picture from picture queue ...");
	int recret = recv(rctx->pictq, videoPicture);
	if (recret == 1) {
		LOG("<== received picture with idx: %d, pts: %0.2fms", videoPicture->idx, videoPicture->pts);
	}
	else if (recret == -1) {
		LOG("<== reveiving picture from picture queue interrupted");
	}
	else {
		LOG("<== unforseen error when receiving picture from picture queue");
	}
}


void
presenter_thread(void *arg)
{
	RendererCtx *rctx = arg;
	AudioSample audioSample;
	VideoPicture videoPicture;
	rctx->audio_start_rt = av_gettime();
	rctx->current_video_time = rctx->audio_start_rt;
	rctx->previous_video_time = rctx->audio_start_rt;

	if (rctx->video_ctx) {
		receive_picture(rctx, &videoPicture);
	}
	for (;;) {
		LOG("receiving sample from audio queue ...");
		int recret = recv(rctx->audioq, &audioSample);
		if (recret != 1) {
			LOG("<== error when receiving sample from audio queue");
			continue;
		}
		LOG("<== received sample with idx: %d, pts: %.2fms from audio queue.", audioSample.idx, audioSample.pts);
		// write audio sample to file after decoding, after resampling, after sending to queue
		//fwrite(audioSample.sample, 1, audioSample.size, audio_out);
		SDL_memset(rctx->mixed_audio_buf, 0, audioSample.size);
		SDL_MixAudioFormat(rctx->mixed_audio_buf, audioSample.sample, rctx->specs.format, audioSample.size, (rctx->audio_vol / 100.0) * SDL_MIX_MAXVOLUME);
		int ret = SDL_QueueAudio(rctx->audio_devid, rctx->mixed_audio_buf, audioSample.size);
		/* int ret = SDL_QueueAudio(rctx->audio_devid, audioSample.sample, audioSample.size); */
		if (ret < 0) {
			LOG("failed to write audio sample: %s", SDL_GetError());
			free(audioSample.sample);
			continue;
		}
		LOG("queued audio sample to sdl device");

		int audioq_size = SDL_GetQueuedAudioSize(rctx->audio_devid);
		int bytes_per_sec = 2 * rctx->audio_ctx->sample_rate * rctx->audio_out_channels;
		double queue_duration = 1000.0 * audioq_size / bytes_per_sec;
		int samples_queued = audioq_size / audioSample.size;
		// current_audio_pts = audioSample.pts - queue_duration;
		double real_time = (av_gettime() - rctx->audio_start_rt) / 1000.0;
		double time_diff = audioSample.pts - real_time;
		LOG("audio sample idx: %d, size: %d", audioSample.idx, audioSample.size);
		LOG("real time: %.2fms, audio pts: %.2fms, video pts %.2fms",
			real_time, audioSample.pts, videoPicture.pts);
		LOG("sdl audio queue size: %d bytes, %.2fms, %d samples",
			audioq_size, queue_duration, samples_queued);
		LOG("AV dist: %.2fms, thresh: %.2fms",
			audioSample.pts - videoPicture.pts, 0.5 * audioSample.duration);

		/* if (samples_queued < 5) { */
			/* LOG("waiting for audio queue to grow ..."); */
			/* yield(); */
			/* sleep(audioSample.sample_duration); */
		/* } */

		if (rctx->video_ctx &&
			fabs(audioSample.pts - videoPicture.pts) <= 0.5 * audioSample.duration)
		{
			receive_picture(rctx, &videoPicture);
			display_picture(rctx, &videoPicture);
			if (videoPicture.frame) {
				av_frame_unref(videoPicture.frame);
				av_frame_free(&videoPicture.frame);
				videoPicture.frame = nil;
			}
		}
		if (time_diff > 0) {
			yield();
			LOG("sleeping %.2fms", time_diff);
			sleep(time_diff);
		}
		free(audioSample.sample);
	}
}


void
display_picture(RendererCtx *rctx, VideoPicture *videoPicture)
{
	if (!videoPicture->frame) {
		LOG("no picture to display");
		return;
	}
	double real_time = (av_gettime() - rctx->audio_start_rt) / 1000.0;
	rctx->previous_video_time = rctx->current_video_time;
	rctx->current_video_time = real_time;
	LOG("displaying picture %d, delta time: %.2fms ...",
		videoPicture->idx,
		rctx->current_video_time - rctx->previous_video_time - rctx->frame_duration);
	int textupd = SDL_UpdateYUVTexture(
			rctx->sdl_texture,
			nil,
			videoPicture->frame->data[0],
			videoPicture->frame->linesize[0],
			videoPicture->frame->data[1],
			videoPicture->frame->linesize[1],
			videoPicture->frame->data[2],
			videoPicture->frame->linesize[2]
	);
	if (textupd != 0) {
		LOG("failed to update sdl texture: %s", SDL_GetError());
	}
	// clear the current rendering target with the drawing color
	SDL_SetRenderDrawColor(rctx->sdl_renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
	SDL_RenderClear(rctx->sdl_renderer);
	// copy and place a portion of the texture to the current rendering target
	// set video size when copying sdl texture to sdl renderer. Texture will be stretched to blit_copy_rect!
	SDL_RenderCopy(rctx->sdl_renderer, rctx->sdl_texture, nil, &rctx->blit_copy_rect);
	// update the screen with any rendering performed since the previous call
	SDL_RenderPresent(rctx->sdl_renderer);
}


void
savePicture(RendererCtx* rctx, VideoPicture *videoPicture, int frameIndex)
{
	AVFrame *frame_rgb = nil;
	uint8_t *buffer = nil;
	if (rctx->frame_fmt == FRAME_FMT_PRISTINE) {
		// Convert the video picture to the target format for saving to disk
	    frame_rgb = av_frame_alloc();
	    int rgb_num_bytes;
	    const int align = 32;
	    rgb_num_bytes = av_image_get_buffer_size(
			AV_PIX_FMT_RGB24,
			videoPicture->width,
			videoPicture->height,
			align
			);
	    buffer = (uint8_t *) av_malloc(rgb_num_bytes * sizeof(uint8_t));
	    av_image_fill_arrays(
			frame_rgb->data,
			frame_rgb->linesize,
			buffer,
			AV_PIX_FMT_RGB24,
			videoPicture->width,
			videoPicture->height,
			align
	    );
		struct SwsContext *rgb_ctx = sws_getContext(
											videoPicture->width,
											videoPicture->height,
											videoPicture->pix_fmt,
											videoPicture->width,
											videoPicture->height,
											AV_PIX_FMT_RGB24,
											SWS_BILINEAR,
											nil,
											nil,
											nil
											);
	    sws_scale(
	        rgb_ctx,
	        (uint8_t const * const *)videoPicture->planes,
	        videoPicture->linesizes,
	        0,
	        videoPicture->height,
	        frame_rgb->data,
	        frame_rgb->linesize
	    );
	}
	LOG("saving video picture to file ...");
	FILE * pFile;
	char szFilename[32];
	int  y;
	// Open file
	sprintf(szFilename, "/tmp/%06d.ppm", frameIndex);
	pFile = fopen(szFilename, "wb");
	if (pFile == nil) {
	    return;
	}
	fprintf(pFile, "P6\n%d %d\n255\n", videoPicture->width, videoPicture->height);
	if (rctx->frame_fmt == FRAME_FMT_PRISTINE) {
	    for (y = 0; y < frame_rgb->height; y++) {
	        fwrite(frame_rgb->data[0] + y * frame_rgb->linesize[0], 1, videoPicture->width * 3, pFile);
	    }
	}
	else if (rctx->frame_fmt == FRAME_FMT_RGB) {
	    for (y = 0; y < videoPicture->height; y++) {
	        fwrite(videoPicture->rgbbuf + y * videoPicture->linesize, 1, videoPicture->width * 3, pFile);
	    }
	}
	fclose(pFile);
	/* av_free_frame(frame_rgb); */
	if (buffer) {
	    av_free(buffer);
	}
	LOG("saved video picture.");
}


void
threadmain(int argc, char **argv)
{
	if (_DEBUG_) {
		chatty9pclient = 1;
		/* chattyfuse = 1; */
	}
	RendererCtx *rctx = av_mallocz(sizeof(RendererCtx));
	reset_rctx(rctx);
	start_server(rctx);
	/* int ret = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER); */
	int ret = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
	if (ret != 0) {
		LOG("Could not initialize SDL - %s", SDL_GetError());
		return;
	}
	if (create_sdl_window(rctx) == -1) {
		return;
	}
	/* setstr(&rctx->fileservername, DEFAULT_SERVER_NAME, 0); */
	blank_window(rctx);
	rctx->cmdq = chancreate(sizeof(Command), MAX_COMMANDQ_SIZE);
	if (argc >= 2) {
		seturl(rctx, argv[1]);
		/* rctx->renderer_state = RSTATE_PLAY; */
		rctx->renderer_state = LOAD;
	}
	audio_out = fopen("/tmp/out.pcm", "wb");
	// start the decoding thread to read data from the AVFormatContext
	rctx->decoder_tid = threadcreate(decoder_thread, rctx, THREAD_STACK_SIZE);
	if (!rctx->decoder_tid) {
		printf("could not start decoder thread: %s.\n", SDL_GetError());
		av_free(rctx);
		return;
	}
	/* av_init_packet(&flush_pkt); */
	/* flush_pkt.data = (uint8_t*)"FLUSH"; */
	for (;;) {
		yield();
		if (rctx->renderer_state == RSTATE_STOP || rctx->renderer_state == RSTATE_PAUSE) {
			sleep(100);
		}
		SDL_Event event;
		ret = SDL_PollEvent(&event);
		if (ret) {
			LOG("received sdl event");
			Command cmd;
			cmd.cmd = CMD_NONE;
			switch(event.type)
			{
				case SDL_KEYDOWN:
				{
					switch(event.key.keysym.sym)
					{
						case SDLK_q:
						{
							cmd.cmd = CMD_QUIT;
						}
						break;
						case SDLK_SPACE:
						{
							cmd.cmd = CMD_PAUSE;
						}
						break;
						case SDLK_s:
						case SDLK_ESCAPE:
						{
							cmd.cmd = CMD_STOP;
						}
						break;
						case SDLK_p:
						case SDLK_RETURN:
						{
							cmd.cmd = CMD_PLAY;
						}
						break;
						case SDLK_RIGHT:
						{
							cmd.cmd = CMD_SEEK;
						}
						break;
					}
					if (cmd.cmd != CMD_NONE) {
						send(rctx->cmdq, &cmd);
					}

				}
				break;
				case SDL_WINDOWEVENT:
				{
					switch(event.window.event)
					{
						case SDL_WINDOWEVENT_RESIZED:
						case SDL_WINDOWEVENT_SIZE_CHANGED:
						case SDL_WINDOWEVENT_MAXIMIZED:
						{
							LOG("window resized");
							resize_video(rctx);
						}
						break;
						case SDL_WINDOWEVENT_SHOWN:
						case SDL_WINDOWEVENT_RESTORED:
						{
							LOG("window restored");
							blank_window(rctx);
						}
					}
				}
				break;
				case SDL_QUIT:
				{
					cmd.cmd = CMD_QUIT;
					send(rctx->cmdq, &cmd);
				}
				break;
			}
		}
	}
	// FIXME never reached ... need to shut down the renderer properly
	LOG("freeing video state");
	av_free(rctx);
	return;
}
