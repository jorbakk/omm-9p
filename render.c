/*
 * Copyright 2022 - 2024 Jörg Bakker
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

#include <u.h>
#include <time.h>  // posix std headers should be included between u.h and libc.h
#include <errno.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include <9pclient.h>

#include <SDL2/SDL.h>

// #include "render.h"
#include "log.h"

#define _DEBUG_ 1
#define DEFAULT_SERVER_NAME ("ommserve")
#define MAX_CMD_STR_LEN 256
#define MAX_COMMANDQ_SIZE (5)
#define THREAD_STACK_SIZE 1024 * 1024 * 10
/// Blocking (synchronous) threads
// #define THREAD_CREATE threadcreate
/// OS pre-emptive threads
#define THREAD_CREATE proccreate
#define VOLPROG "pactl"
#define HW_MASTER_VOL

#ifdef RENDER_DUMMY
#elif defined RENDER_VLC
#include "vlc/vlc.h"
// #include <SDL2/SDL_mutex.h>
#include <SDL2/SDL_syswm.h>
#elif defined RENDER_FFMPEG
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#define MAX_AUDIO_FRAME_SIZE 192000
#endif   /// RENDER_DUMMY

typedef struct RendererCtx
{
	// State machine
	int                renderer_state;
	int                next_renderer_state;
	int                quit;
	Channel           *cmdq;
	// Input file name, plan 9 file reference
	char              *url;
	char              *fileservername;
	char              *filename;
	int                isfile;
	int                isaddr;
	int                fileserverfd;
	CFid              *fileserverfid;
	CFsys             *fileserver;
	// Seeking
	int	               seek_req;
	int	               seek_flags;
	int64_t            seek_pos;
	// OS window
	int                screen_width;
	int                screen_height;
	int                window_width;
	int                window_height;
	int                w, h, aw, ah;
	// Threads
	int                server_tid;
	int                decoder_tid;
	int                presenter_tid;
	int                pause_presenter_thread;
	Channel           *presq;
	// Audio output
	SDL_AudioDeviceID  audio_devid;
	int                audio_only;
#ifdef RENDER_FFMPEG
	// Decoder context
	AVIOContext       *io_ctx;
	AVFormatContext   *format_ctx;
	AVCodecContext    *current_codec_ctx;
	AVFrame           *decoder_frame;
	AVPacket          *decoder_packet;
	// Audio stream
	int                audio_stream;
	AVCodecContext    *audio_ctx;
	Channel           *audioq;
	uint8_t            mixed_audio_buf[MAX_AUDIO_FRAME_SIZE];
	unsigned int       audio_buf_size;
	unsigned int       audio_buf_index;
	int                audio_idx;
	double             audio_pts;
	int                audio_out_channels;
	int64_t            audio_start_rt;
	AVRational         audio_timebase;
	double             audio_tbd;
	int                audio_vol;
	SwrContext        *swr_ctx;
	// Video stream
	int                video_stream;
	double             frame_rate;
	double             frame_duration;
	AVCodecContext    *video_ctx;
	Channel           *pictq;
	struct SwsContext *yuv_ctx;
	SDL_AudioSpec      specs;
	int                video_idx;
	double             video_pts;
	uint8_t           *yuvbuffer;
	SDL_Rect           blit_copy_rect;
	AVRational         video_timebase;
	double             video_tbd;
	// Video output
	SDL_Window        *sdl_window;
	SDL_Renderer      *sdl_renderer;
	SDL_Texture       *sdl_texture;
#elif defined RENDER_VLC
	libvlc_instance_t     *libvlc;
	libvlc_media_t        *media;
	libvlc_media_player_t *player;
	// Video output
	SDL_Window            *sdl_window;
	SDL_Renderer      *sdl_renderer;
#endif   /// RENDER_FFMPEG
} RendererCtx;

static RendererCtx rctx;

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

static char* statestr[NSTATE] =
{
	"STOP",
	"RUN",
	"IDLE",
	"LOAD",
	"UNLOAD",
	"ENGAGE",
	"DISENGAGE",
	"EXIT",
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
};

typedef struct Command
{
	int         cmd;
	char       *arg;
	int         argn;
} Command;

#define NCMD 9
typedef void (*cmd_func)(RendererCtx*, char*, int);

void cmd_put(RendererCtx *rctx, char *arg, int argn);
void cmd_stop(RendererCtx *rctx, char *arg, int argn);
void cmd_play(RendererCtx *rctx, char *arg, int argn);
void cmd_pause(RendererCtx *rctx, char *arg, int argn);
void cmd_quit(RendererCtx *rctx, char *arg, int argn);
void cmd_seek(RendererCtx *rctx, char *arg, int argn);
void cmd_vol(RendererCtx *rctx, char *arg, int argn);

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
	cmd_put,
	nil,
	nil,
	nil,
	cmd_quit,
	cmd_seek,
	cmd_vol,
	nil,
	nil
};

static char* cmdstr[NCMD] =
{
	"put",
	"stop",
	"play",
	"pause",
	"quit",
	"seek",
	"vol",
	"none",
	"err",
};

// Transitions is the State matrix with dimensions [cmds x current state]
// Each entry is the *next* state when cmd is received in current state.
// Commands in states LOAD, UNLOAD, ENGAGE, DISENG are ignored because
// read_cmd() is not called
static int transitions[NCMD][NSTATE-1] = // no entry for EXIT state needed
{
// Current state:
//   STOP,    RUN,     IDLE,    LOAD,    UNLOAD,  ENGAGE,  DISENG
// CMD_SET
	{STOP,    RUN,     IDLE,    RUN,     STOP,    RUN,     IDLE},
// CMD_STOP
	{STOP,    UNLOAD,  UNLOAD,  RUN,     STOP,    RUN,     IDLE},
// CMD_PLAY
	{LOAD,    RUN,     ENGAGE,  RUN,     STOP,    RUN,     IDLE},
// CMD_PAUSE
	{STOP,    DISENG,  ENGAGE,  RUN,     STOP,    RUN,     IDLE},
// CMD_QUIT, exiting only in state STOP possible, all others keep the state
	{EXIT,    RUN,     IDLE,    RUN,     STOP,    RUN,     IDLE},
// CMD_SEEK
	{STOP,    RUN,     IDLE,    RUN,     STOP,    RUN,     IDLE},
// CMD_VOL
	{STOP,    RUN,     IDLE,    RUN,     STOP,    RUN,     IDLE},
// CMD_NONE, unconditional straight transitions
	{STOP,    RUN,     IDLE,    RUN,     STOP,    RUN,     IDLE},
// CMD_ERR, error occured while running the state
	{STOP,    UNLOAD,  IDLE,    RUN,     STOP,    RUN,     IDLE},
};

/// Function declarations
void decoder_thread(void *arg);
int  resize_video(RendererCtx *rctx);
int  create_window(RendererCtx *rctx);
void close_window(RendererCtx *rctx);
void wait_for_window_resize(RendererCtx *rctx);
void blank_window(RendererCtx *rctx);
int  read_cmd(RendererCtx *rctx, int mode);
void seturl(RendererCtx *rctx, char *url);

/// Implementation
void
reset_rctx(RendererCtx *rctx, int init)
{
	if (init) {
		rctx->url = nil;
		rctx->fileservername = nil;
		rctx->filename = nil;
		rctx->isfile = 0;
		rctx->isaddr = 0;
		rctx->fileserverfd = -1;
		rctx->fileserverfid = nil;
		rctx->fileserver = nil;
	}
	if (init) {
		/* rctx->av_sync_type = DEFAULT_AV_SYNC_TYPE; */
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
	}
	rctx->presenter_tid = 0;
	rctx->pause_presenter_thread = 0;
	// Presenting
	rctx->presq = nil;
	// Seeking
	rctx->seek_req = 0;
	rctx->seek_flags = 0;
	rctx->seek_pos = 0;
	rctx->audio_devid = -1;
	rctx->audio_only = 0;
#ifdef RENDER_FFMPEG
	rctx->io_ctx = nil;
	rctx->format_ctx = nil;
	rctx->current_codec_ctx = nil;
	rctx->decoder_frame = nil;
	rctx->decoder_packet = nil;
	// Audio stream
	rctx->audio_stream = -1;
	rctx->audio_ctx = nil;
	rctx->audioq = nil;
	rctx->audio_buf_size = 0;
	rctx->audio_buf_index = 0;
	rctx->audio_idx = 0;
	rctx->audio_pts = 0.0;
	rctx->audio_out_channels = 2;
	rctx->audio_start_rt = 0;
	rctx->audio_timebase.num = 0;
	rctx->audio_timebase.den = 0;
	rctx->audio_tbd = 0.0;
	if (init) {
		rctx->audio_vol = 100;
	}
	rctx->swr_ctx = nil;
	// Video stream.
	if (init) {
		rctx->sdl_window = nil;
		rctx->sdl_texture = nil;
		rctx->sdl_renderer = nil;
	}
	rctx->video_stream = -1;
	rctx->frame_rate = 0.0;
	rctx->frame_duration = 0.0;
	rctx->video_ctx = nil;
	rctx->pictq = nil;
	rctx->yuv_ctx = nil;
	rctx->video_idx = 0;
	rctx->video_pts = 0.0;
	rctx->yuvbuffer = nil;
	if (init) {
		rctx->w = 0;
		rctx->h = 0;
		rctx->aw = 0;
		rctx->ah = 0;
	}
	rctx->video_timebase.num = 0;
	rctx->video_timebase.den = 0;
	rctx->video_tbd = 0.0;
#endif   /// RENDER_FFMPEG
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


// void
// srvread(Req *r)
// {
	// LOG("server read");
	// r->ofcall.count = 6;
	// r->ofcall.data = "hello\n";
	// respond(r, nil);
// }


void
srvwrite(Req *r)
{
	LOG("server write");
	Command command = {.cmd = CMD_NONE, .arg = nil, .argn = 0};
	char cmdbuf[MAX_CMD_STR_LEN];
	if (r->ifcall.count > MAX_CMD_STR_LEN - 1) LOG("error: received command too long");
	snprint(cmdbuf, r->ifcall.count, "%s", r->ifcall.data);
	int cmdlen = r->ifcall.count;
	int arglen = 0;
	char* argstr = strchr(cmdbuf, ' ');
	if (argstr != nil) {
		cmdlen = argstr - cmdbuf;
		arglen = r->ifcall.count - cmdlen + 1;
		*argstr = '\0';
		argstr++;
		LOG("server cmd: %s [%d], arg: %s [%d]", cmdbuf, cmdlen, argstr, arglen);
		command.argn = arglen;
		// command.arg = calloc(arglen, sizeof(char));
		command.arg = malloc(arglen);
		memcpy(command.arg, argstr, arglen);
		// command.arg[arglen] = '\0';
	}
	else {
		LOG("server cmd: %s", cmdbuf);
	}
	for (int i=0; i<NCMD; ++i) {
		if (strncmp(cmdbuf, cmdstr[i], strlen(cmdstr[i])) == 0) {
			command.cmd = i;
		}
	}
	RendererCtx *rctx = r->fid->file->aux;
	if (rctx) {
		LOG("queueing command: %d (%s) ...", command.cmd, cmdbuf);
		send(rctx->cmdq, &command);
	}
	else {
		LOG("server file has no renderer context");
	}
	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}


Srv server = {
	.open  = srvopen,
	/* .read  = srvread, */
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
	char *srvname = "ommrender";
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
	// server.foreground = 1;
	threadpostmountsrv(&server, srvname, mtpt, MREPL|MCREATE);
	/* threadexits(0); */
	LOG("9P server started.");
}


void
stop_server(RendererCtx *rctx)
{
	reset_filectx(rctx);
	threadexitsall("");
}


void
state_stop(RendererCtx *rctx)
{
	blank_window(rctx);
	while (read_cmd(rctx, READCMD_BLOCK) == KEEP_STATE) {
	}
}


void
state_idle(RendererCtx *rctx)
{
	while (read_cmd(rctx, READCMD_BLOCK) == KEEP_STATE) {
	}
}


void
state_exit(RendererCtx *rctx)
{
	rctx->quit = 1;
	stop_server(rctx);
}


void
cmd_put(RendererCtx *rctx, char *arg, int argn)
{
	if (argn == 0) {
		LOG("put needs an argument, ignoring");
		return;
	}
	setstr(&rctx->url, arg, argn);
	seturl(rctx, rctx->url);
}


void
cmd_quit(RendererCtx *rctx, char *arg, int argn)
{
	(void)rctx; (void)arg; (void)argn;
	// currently nothing planned here ...
}


#ifdef HW_MASTER_VOL
void
cmd_vol(RendererCtx *rctx, char *arg, int argn)
{
	(void)rctx; (void)argn;
	char volprogcmd[128];
	snprintf(volprogcmd, 128, VOLPROG " set-sink-volume @DEFAULT_SINK@ %s%%", arg);
	int ret = system(volprogcmd);
	if (ret == -1 || ret == 127) {
		LOG("failed to execute %s for setting the hardware master volume: %s", VOLPROG, strerror(errno));
	} else {
		LOG("%s returned: %d", VOLPROG, ret);
	}
}
#endif


int
read_cmd(RendererCtx *rctx, int mode)
{
	int ret;
	Command command = {.cmd = -1, .arg = nil, .argn = 0};
	if (mode == READCMD_BLOCK) {
		ret = recv(rctx->cmdq, &command);
	}
	else if (mode == READCMD_POLL) {
		ret = nbrecv(rctx->cmdq, &command);
	}
	else {
		LOG("unsupported read command mode");
		return KEEP_STATE;
	}
	if (ret == -1) {
		/* LOG("receiving command interrupted"); */
		return KEEP_STATE;
	}
	if (ret == 1) {
		LOG("<== received command: %d (%s) with arg: %s",
			command.cmd, cmdstr[command.cmd], command.arg);
		if (cmds[command.cmd] == nil) {
			LOG("command is nil, nothing to execute");
		}
		else {
			cmds[command.cmd](rctx, command.arg, command.argn);
		}
		if (command.arg != nil) {
			free(command.arg);
			command.arg = nil;
		}
		int next_renderer_state = transitions[command.cmd][rctx->renderer_state];
		LOG("state: %d (%s) -> %d (%s)",
			rctx->renderer_state,
			statestr[rctx->renderer_state],
			next_renderer_state,
			statestr[next_renderer_state]);
		if (next_renderer_state == rctx->renderer_state) {
			return KEEP_STATE;
		}
		rctx->renderer_state = next_renderer_state;
		return CHANGE_STATE;
	}
	return KEEP_STATE;
}


void
decoder_thread(void *arg)
{
	RendererCtx *rctx = (RendererCtx *)arg;
	LOG("decoder thread started with id: %d", rctx->decoder_tid);
	// Initial renderer state is LOAD only if an url was given as a command line argument
	// otherwise it defaults to STOP
	while(!rctx->quit) {
		LOG("entering state %d (%s)", rctx->renderer_state, statestr[rctx->renderer_state]);
		states[rctx->renderer_state](rctx);
	}
}


void
threadmain(int argc, char **argv)
{
	if (_DEBUG_) {
		chatty9pclient = 1;
		/* chattyfuse = 1; */
	}
	reset_rctx(&rctx, 1);
	// Create OS window
	/* int ret = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER); */
	int ret = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
	if (ret != 0) {
		LOG("Could not initialize SDL - %s", SDL_GetError());
		return;
	}
	if (create_window(&rctx) == -1) {
		return;
	}
	blank_window(&rctx);
	// Wait for sdl window to be created (restored) and resized
	wait_for_window_resize(&rctx);
	// Start command server
	rctx.cmdq = chancreate(sizeof(Command), MAX_COMMANDQ_SIZE);
	start_server(&rctx);
	// Load file if url is given on command line
	if (argc >= 2) {
		seturl(&rctx, argv[1]);
		rctx.renderer_state = LOAD;
	}
	// Start decoder / state machine thread
	rctx.decoder_tid = THREAD_CREATE(decoder_thread, &rctx, THREAD_STACK_SIZE);
	if (!rctx.decoder_tid) {
		printf("could not start decoder thread\n");
		return;
	}
	SDL_Event event;
	for (;;) {
		yield();
		if (rctx.renderer_state == STOP || rctx.renderer_state == IDLE) {
			sleep(100);
		}
		ret = SDL_PollEvent(&event);
		if (ret) {
			LOG("received sdl event type: %d", event.type);
			Command command = { CMD_NONE, nil, 0 };
			switch(event.type)
			{
				case SDL_KEYDOWN:
				{
					switch(event.key.keysym.sym)
					{
						case SDLK_q:
						{
							command.cmd = CMD_QUIT;
						}
						break;
						case SDLK_SPACE:
						{
							command.cmd = CMD_PAUSE;
						}
						break;
						case SDLK_s:
						case SDLK_ESCAPE:
						{
							command.cmd = CMD_STOP;
						}
						break;
						case SDLK_p:
						case SDLK_RETURN:
						{
							command.cmd = CMD_PLAY;
						}
						break;
						case SDLK_RIGHT:
						{
							command.cmd = CMD_SEEK;
						}
						break;
					}
					if (command.cmd != CMD_NONE) {
						send(rctx.cmdq, &command);
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
							resize_video(&rctx);
						}
						break;
						case SDL_WINDOWEVENT_SHOWN:
						case SDL_WINDOWEVENT_RESTORED:
						{
							LOG("window restored");
							blank_window(&rctx);
						}
					}
				}
				break;
				case SDL_QUIT:
				{
					command.cmd = CMD_QUIT;
					send(rctx.cmdq, &command);
				}
				break;
			}
		}
	}
	// FIXME never reached ... need to shut down the renderer properly
	LOG("freeing video state");
	return;
}

#ifdef RENDER_DUMMY
#include "render_dummy.c"
#elif defined RENDER_VLC
#include "render_vlc.c"
#elif defined RENDER_FFMPEG
#include "render_ffmpeg.c"
#endif   /// RENDER_DUMMY
