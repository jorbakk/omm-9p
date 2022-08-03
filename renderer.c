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
// - mp2 audio track on dvb live ts streams
// - aspect ratio in half screen sized vertical windows
// - memory leaks
// - seek
// - blanking screen on stop / eof
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
// 8. Support local files, not only 9P server (maybe also other protocols like http ...)

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

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/avstring.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#include <SDL2/SDL.h>


#define LOG(...) printloginfo(); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");

static struct timespec curtime;
static FILE *audio_out;

void printloginfo(void)
{
	long            ms; // Milliseconds
	time_t          s;  // Seconds
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
	// Input file name, plan 9 file reference, os window
	char              *url;
	char              *fileservername;
	char              *filename;
	int                isaddr;
	int                fileserverfd;
	CFid              *fileserverfid;
	CFsys             *fileserver;
	AVIOContext       *io_ctx;
	AVFormatContext   *format_ctx;
	AVCodecContext    *current_codec_ctx;
	int                renderer_state;
	Channel           *cmdq;
	int                screen_width;
	int                screen_height;
	int                window_width;
	int                window_height;
	// Threads
	int                server_tid;
	int                decoder_tid;
	int                presenter_tid;
	// Audio Stream.
	int                audio_stream;
	AVCodecContext    *audio_ctx;
	Channel           *audioq;
	uint8_t            audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) /2];
	uint8_t            mixed_audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) /2];
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
	// Video Stream.
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
	SDL_AudioDeviceID  audio_devid;
	int                audio_only;
} RendererCtx;

typedef struct AudioResamplingState
{
	SwrContext * swr_ctx;
	int64_t in_channel_layout;
	uint64_t out_channel_layout;
	int out_nb_channels;
	int out_linesize;
	int in_nb_samples;
	int64_t out_nb_samples;
	int64_t max_out_nb_samples;
	uint8_t ** resampled_data;
	int resampled_data_size;

} AudioResamplingState;

typedef struct Command
{
	int         cmd;
	char       *arg;
	int         narg;
} Command;

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
	/* uint8_t			 sample[(MAX_AUDIO_FRAME_SIZE * 3) /2]; */
	uint8_t	   *sample;
	int         size;
	int         idx;
	double      pts;
	double      duration;
} AudioSample;

enum
{
	RSTATE_STOP,
	RSTATE_PLAY,
	RSTATE_PAUSE,
	RSTATE_SEEK,
	RSTATE_QUIT,
};

enum
{
	CMD_STOP,
	CMD_PLAY,
	CMD_PAUSE,
	CMD_SEEK,
	CMD_QUIT,
	CMD_SET,
	CMD_VOL,
	CMD_NONE,
};

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

/* AVPacket flush_pkt; */

void display_picture(RendererCtx *rctx, VideoPicture *videoPicture);
void decoder_thread(void *arg);
void presenter_thread(void *arg);
void blank_window(RendererCtx *rctx);


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
parseurl(char *url, char **fileservername, char **filename, int *isaddr)
{
	char *pfileservername = nil;
	char *pfilename = nil;
	char *pbang = strchr(url, '!');
	char *pslash = strchr(url, '/');
	int fisaddr = 0;
	if (pslash == nil) {
		if (pbang != nil) {
			return -1;
		}
		pfilename = url;
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
	return 0;
}


void
seturl(RendererCtx *rctx, char *url)
{
	/* setstr(&rctx->url, url, 0); */
	char *s, *f;
	int ret = parseurl(rctx->url, &s, &f, &rctx->isaddr);
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


int
read_cmd(RendererCtx *rctx)
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
poll_cmd(RendererCtx *rctx)
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
	int aw = h / far;
	int ah = h;
	if (war > far) {
		aw = w;
		ah = w * far;
	}
	rctx->aw = aw;
	rctx->ah = ah;
	LOG("scaling frame: %dx%d to win size: %dx%d, aspect ratio win: %f, aspect ratio frame: %f, final picture size: %dx%d", rctx->video_ctx->width, rctx->video_ctx->height, w, h, war, far, aw, ah);
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
	unsigned char *avctxBuffer;
	avctxBuffer = malloc(avctxBufferSize);
	AVIOContext *io_ctx = avio_alloc_context(
		avctxBuffer,                   // buffer
		avctxBufferSize,               // buffer size
		0,                             // buffer is only readable - set to 1 for read/write
		rctx->fileserverfid,   // user specified data
		demuxerPacketRead,             // function for reading packets
		nil,                          // function for writing packets
		demuxerPacketSeek              // function for seeking to position in stream
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
	int ret = avformat_open_input(&rctx->format_ctx, nil, nil, nil);
	if (ret < 0) {
		LOG("Could not open file %s", rctx->filename);
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
open_stream_component(RendererCtx *rctx, int stream_index)
{
	LOG("opening stream component ...");
	AVFormatContext *format_ctx = rctx->format_ctx;
	if (stream_index < 0 || stream_index >= format_ctx->nb_streams) {
		LOG("Invalid stream index");
		return -1;
	}
	AVCodec *codec = avcodec_find_decoder(format_ctx->streams[stream_index]->codecpar->codec_id);
	if (codec == nil) {
		LOG("Unsupported codec");
		return -1;
	}
	AVCodecContext *codecCtx = avcodec_alloc_context3(codec);
	int ret = avcodec_parameters_to_context(codecCtx, format_ctx->streams[stream_index]->codecpar);
	if (ret != 0) {
		LOG("Could not copy codec context");
		return -1;
	}
	if (codecCtx->codec_type == AVMEDIA_TYPE_AUDIO) {
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
			printf("SDL_OpenAudio: %s.\n", SDL_GetError());
			return -1;
		}
		LOG("audio device with id: %d opened successfully", rctx->audio_devid);
		LOG("audio specs are freq: %d, channels: %d", rctx->specs.freq, rctx->specs.channels);
	}
	if (avcodec_open2(codecCtx, codec, nil) < 0) {
		LOG("Unsupported codec");
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
			/* rctx->audioq = chancreate(sizeof(AVPacket*), MAX_AUDIOQ_SIZE); */
			rctx->audioq = chancreate(sizeof(AVPacket), MAX_AUDIOQ_SIZE);
			rctx->presenter_tid = threadcreate(presenter_thread, rctx, THREAD_STACK_SIZE);
			LOG("presenter thread created with id: %i", rctx->presenter_tid);
			LOG("calling sdl_pauseaudio(0) ...");
			/* SDL_PauseAudio(0); */
			SDL_PauseAudioDevice(rctx->audio_devid, 0);
			LOG("sdl_pauseaudio(0) called.");
			rctx->audio_timebase = rctx->format_ctx->streams[stream_index]->time_base;
			rctx->audio_tbd = av_q2d(rctx->audio_timebase);
			LOG("timebase of audio stream: %d/%d = %f",
				rctx->audio_timebase.num, rctx->audio_timebase.den, rctx->audio_tbd);
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
		}
			break;
		default:
		{
			// nothing to do
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
	int video_stream = -1;
	int audio_stream = -1;
	for (int i = 0; i < rctx->format_ctx->nb_streams; i++)
	{
		if (rctx->format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream < 0) {
			video_stream = i;
			LOG("selecting stream %d for video", video_stream);
		}
		if (rctx->format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream < 0) {
			audio_stream = i;
			LOG("selecting stream %d for audio", audio_stream);
		}
	}
	if (video_stream == -1) {
		LOG("Could not find video stream.");
	}
	else {
		ret = open_stream_component(rctx, video_stream);
		if (ret < 0) {
			printf("Could not open video codec.\n");
			return -1;
		}
		LOG("video stream component opened successfully.");
	}
	if (audio_stream == -1) {
		LOG("Could not find audio stream.");
	}
	else {
		ret = open_stream_component(rctx, audio_stream);
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
	LOG("read packet with size: %d, pts: %ld, dts: %ld, duration: %ld, pos: %ld",
		packet->size, packet->pts, packet->dts, packet->duration, packet->pos);
	double tbdms = 1000 * ((packet->stream_index == rctx->audio_stream) ?
		rctx->audio_tbd : rctx->video_tbd);
	char *stream = (packet->stream_index == rctx->audio_stream) ? "audio" : "video";
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
	else {
		LOG("sending audio packet of size %d to decoder", packet->size);
		codecCtx = rctx->audio_ctx;
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
		// FIXME sending audio packet to decoder fails with arte.ts
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


AudioResamplingState*
getAudioResampling(uint64_t channel_layout)
{
	AudioResamplingState *audioResampling = av_mallocz(sizeof(AudioResamplingState));
	audioResampling->swr_ctx = swr_alloc();
	audioResampling->in_channel_layout = channel_layout;
	audioResampling->out_channel_layout = AV_CH_LAYOUT_STEREO;
	audioResampling->out_nb_channels = 0;
	audioResampling->out_linesize = 0;
	audioResampling->in_nb_samples = 0;
	audioResampling->out_nb_samples = 0;
	audioResampling->max_out_nb_samples = 0;
	audioResampling->resampled_data = nil;
	audioResampling->resampled_data_size = 0;
	return audioResampling;
}


int 
resample_audio(RendererCtx *rctx, AVFrame *decoded_audio_frame, enum AVSampleFormat out_sample_fmt, uint8_t *out_buf)
{
	// get an instance of the AudioResamplingState struct
	AudioResamplingState * arState = getAudioResampling(rctx->audio_ctx->channel_layout);
	if (!arState->swr_ctx) {
		LOG("swr_alloc error");
		return -1;
	}
	// get input audio channels
	arState->in_channel_layout = (rctx->audio_ctx->channels ==
								  av_get_channel_layout_nb_channels(rctx->audio_ctx->channel_layout)) ?
								 rctx->audio_ctx->channel_layout :
								 av_get_default_channel_layout(rctx->audio_ctx->channels);
	// check input audio channels correctly retrieved
	if (arState->in_channel_layout <= 0) {
		LOG("in_channel_layout error");
		return -1;
	}
	// set output audio channels based on the input audio channels
	if (rctx->audio_ctx->channels == 1) {
		arState->out_channel_layout = AV_CH_LAYOUT_MONO;
	}
	else if (rctx->audio_ctx->channels == 2) {
		arState->out_channel_layout = AV_CH_LAYOUT_STEREO;
	}
	else {
		arState->out_channel_layout = AV_CH_LAYOUT_SURROUND;
	}
	// retrieve number of audio samples (per channel)
	arState->in_nb_samples = decoded_audio_frame->nb_samples;
	if (arState->in_nb_samples <= 0) {
		LOG("in_nb_samples error");
		return -1;
	}
	// Set SwrContext parameters for resampling
	av_opt_set_int(
			arState->swr_ctx,
			"in_channel_layout",
			arState->in_channel_layout,
			0
	);
	// Set SwrContext parameters for resampling
	av_opt_set_int(
			arState->swr_ctx,
			"in_sample_rate",
			rctx->audio_ctx->sample_rate,
			0
	);
	// Set SwrContext parameters for resampling
	av_opt_set_sample_fmt(
			arState->swr_ctx,
			"in_sample_fmt",
			rctx->audio_ctx->sample_fmt,
			0
	);
	// Set SwrContext parameters for resampling
	av_opt_set_int(
			arState->swr_ctx,
			"out_channel_layout",
			arState->out_channel_layout,
			0
	);
	// Set SwrContext parameters for resampling
	av_opt_set_int(
			arState->swr_ctx,
			"out_sample_rate",
			rctx->audio_ctx->sample_rate,
			0
	);
	// Set SwrContext parameters for resampling
	av_opt_set_sample_fmt(
			arState->swr_ctx,
			"out_sample_fmt",
			out_sample_fmt,
			0
	);
	// initialize SWR context after user parameters have been set
	int ret = swr_init(arState->swr_ctx);
	if (ret < 0) {
		LOG("Failed to initialize the resampling context");
		return -1;
	}
	arState->max_out_nb_samples = arState->out_nb_samples = av_rescale_rnd(
			arState->in_nb_samples,
			rctx->audio_ctx->sample_rate,
			rctx->audio_ctx->sample_rate,
			AV_ROUND_UP
	);
	// check rescaling was successful
	if (arState->max_out_nb_samples <= 0) {
		LOG("av_rescale_rnd error");
		return -1;
	}
	// get number of output audio channels
	arState->out_nb_channels = av_get_channel_layout_nb_channels(arState->out_channel_layout);
	// allocate data pointers array for arState->resampled_data and fill data
	// pointers and linesize accordingly
	ret = av_samples_alloc_array_and_samples(
			&arState->resampled_data,
			&arState->out_linesize,
			arState->out_nb_channels,
			arState->out_nb_samples,
			out_sample_fmt,
			0
	);
	// check memory allocation for the resampled data was successful
	if (ret < 0) {
		LOG("av_samples_alloc_array_and_samples() error: Could not allocate destination samples");
		return -1;
	}
	// retrieve output samples number taking into account the progressive delay
	arState->out_nb_samples = av_rescale_rnd(
			swr_get_delay(arState->swr_ctx, rctx->audio_ctx->sample_rate) + arState->in_nb_samples,
			rctx->audio_ctx->sample_rate,
			rctx->audio_ctx->sample_rate,
			AV_ROUND_UP
	);
	// check output samples number was correctly rescaled
	if (arState->out_nb_samples <= 0) {
		LOG("av_rescale_rnd error");
		return -1;
	}
	if (arState->out_nb_samples > arState->max_out_nb_samples) {
		// free memory block and set pointer to nil
		av_free(arState->resampled_data[0]);
		// Allocate a samples buffer for out_nb_samples samples
		ret = av_samples_alloc(
				arState->resampled_data,
				&arState->out_linesize,
				arState->out_nb_channels,
				arState->out_nb_samples,
				out_sample_fmt,
				1
		);
		// check samples buffer correctly allocated
		if (ret < 0) {
			LOG("av_samples_alloc failed");
			return -1;
		}
		arState->max_out_nb_samples = arState->out_nb_samples;
	}
	if (arState->swr_ctx) {
		// do the actual audio data resampling
		ret = swr_convert(
				arState->swr_ctx,
				arState->resampled_data,
				arState->out_nb_samples,
				(const uint8_t **) decoded_audio_frame->data,
				decoded_audio_frame->nb_samples
		);
		// check audio conversion was successful
		if (ret < 0) {
			LOG("swr_convert_error");
			return -1;
		}
		// get the required buffer size for the given audio parameters
		arState->resampled_data_size = av_samples_get_buffer_size(
				&arState->out_linesize,
				arState->out_nb_channels,
				ret,
				out_sample_fmt,
				1
		);
		// check audio buffer size
		if (arState->resampled_data_size < 0) {
			LOG("av_samples_get_buffer_size error");
			return -1;
		}
	}
	else {
		LOG("swr_ctx null error");
		return -1;
	}
	// copy the resampled data to the output buffer
	memcpy(out_buf, arState->resampled_data[0], arState->resampled_data_size);
	if (arState->resampled_data) {
		// free memory block and set pointer to nil
		av_freep(&arState->resampled_data[0]);
	}
	av_freep(&arState->resampled_data);
	arState->resampled_data = nil;
	if (arState->swr_ctx) {
		// free the allocated SwrContext and set the pointer to nil
		swr_free(&arState->swr_ctx);
	}
	return arState->resampled_data_size;
}


int
create_sample_from_frame(RendererCtx *rctx, AVFrame *frame, AudioSample *audioSample)
{
	int bytes_per_sec = 2 * rctx->current_codec_ctx->sample_rate * rctx->audio_out_channels;
	int data_size = resample_audio(
			rctx,
			frame,
			AV_SAMPLE_FMT_S16,
			rctx->audio_buf);
	double sample_duration = 1000.0 * data_size / bytes_per_sec;
	audioSample->size = data_size;
	audioSample->duration = sample_duration;
	LOG("resampled audio bytes: %d", data_size);
	memcpy(audioSample->sample, rctx->audio_buf, sizeof(rctx->audio_buf));
	/* double sample_duration = 0.5 * 1000.0 * audioSample->size / bytes_per_sec; */
	/* double sample_duration = 2 * 1000.0 * audioSample->size / bytes_per_sec; */
	LOG("audio sample rate: %d, channels: %d, duration: %.2fms",
		rctx->current_codec_ctx->sample_rate, rctx->audio_out_channels, audioSample->duration);
	/* rctx->audio_pts += sample_duration;  // audio sample length in ms */
	return 0;
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


void
decoder_thread(void *arg)
{
	RendererCtx *rctx = (RendererCtx *)arg;
	LOG("decoder thread started with id: %d", rctx->decoder_tid);

start:
	// FIXME handle renderer state properly in any situation
	rctx->renderer_state = RSTATE_STOP;
	if (read_cmd(rctx) == 1) {
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
		int jmp = poll_cmd(rctx);
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
				LOG("writing pcm sample of size %d to file", frame->linesize[0]);
				fwrite(frame->data,
					/* rctx->current_codec_ctx->channels * rctx->current_codec_ctx->bits_per_raw_sample / 8, */
					/* rctx->current_codec_ctx->frame_size, */
					1,
					frame->linesize[0],
					/* frame->nb_samples, */
					audio_out);

				rctx->audio_idx++;
				AudioSample audioSample = {
					.idx = rctx->audio_idx,
					.sample = malloc(sizeof(rctx->audio_buf)),
					};
				if (create_sample_from_frame(rctx, frame, &audioSample) == 2) {
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
reset_rctx(RendererCtx *rctx)
{
	rctx->url = nil;
	rctx->fileservername = nil;
	rctx->filename = nil;
	rctx->isaddr = 0;
	rctx->fileserverfd = -1;
	rctx->fileserverfid = nil;
	rctx->fileserver = nil;
	rctx->io_ctx = nil;
	rctx->format_ctx = nil;
	rctx->current_codec_ctx = nil;
	/* rctx->av_sync_type = DEFAULT_AV_SYNC_TYPE; */
	rctx->renderer_state = RSTATE_STOP;
	rctx->cmdq = nil;
	rctx->screen_width = 0;
	rctx->screen_height = 0;
	rctx->window_width = 0;
	rctx->window_height = 0;
	rctx->server_tid = 0;
	rctx->decoder_tid = 0;
	rctx->presenter_tid = 0;
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
	// Video Stream.
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
			p9sleep(100);
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
