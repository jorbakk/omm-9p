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
// 1. Seek / audio volume control
// 2. Fixes / refactoring / testing
// - fix memory leaks
// - allow video scaling not only in decoder thread but also in presenter thread
// - blanking screen on stop / eof doesn't work allways
// - test keyboard / server input combinations (fuzz testing ...)
// 3. AV sync
// - decrease video picture display rate variation further
// - remove audio delay (... if there's any ... caused by samples in sdl queue?!)
// - add video-only (for videos with or w/o audio) and fix audio-only video playback
// 4. Query renderer info (current position, state, audio volume) from 9P server
// 5. Display single still images
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

// FIXME saving frames with FRAME_FMT_PRISTINE = 1 results in random noise images,
//       FRAME_FMT_PRISTINE = 0 crashes
#define FRAME_FMT_PRISTINE 1
#define FRAME_FMT_RGB 1
#define FRAME_FMT_YUV 2


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
	CMD_NONE,
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

void saveFrame(AVFrame *pFrame, int width, int height, int frameIndex);
void display_picture(RendererCtx *renderer_ctx, VideoPicture *videoPicture);
void savePicture(RendererCtx *renderer_ctx, VideoPicture *pPic, int frameIndex);
int open_stream_component(RendererCtx * renderer_ctx, int stream_index);
void decoder_thread(void *arg);
void presenter_thread(void *arg);
void blank_window(RendererCtx *renderer_ctx);

static int resample_audio(
		RendererCtx *renderer_ctx,
		AVFrame * decoded_audio_frame,
		enum AVSampleFormat out_sample_fmt,
		uint8_t * out_buf
);
AudioResamplingState * getAudioResampling(uint64_t channel_layout);
void stream_seek(RendererCtx *renderer_ctx, int64_t pos, int rel);


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
seturl(RendererCtx *renderer_ctx, char *url)
{
	/* setstr(&renderer_ctx->url, url, 0); */
	char *s, *f;
	int ret = parseurl(renderer_ctx->url, &s, &f, &renderer_ctx->isaddr);
	if (ret == -1) {
		LOG("failed to parse url %s", url);
		renderer_ctx->fileservername = nil;
		renderer_ctx->filename = nil;
		return;
	}
	setstr(&renderer_ctx->fileservername, s, 0);
	setstr(&renderer_ctx->filename, f, 0);
}


int
clientdial(RendererCtx *renderer_ctx)
{
	LOG("dialing address: %s ...", renderer_ctx->fileservername);
	if ((renderer_ctx->fileserverfd = dial(renderer_ctx->fileservername, nil, nil, nil)) < 0)
		return -1;
		/* sysfatal("dial: %r"); */
	LOG("mounting address ...");
	if ((renderer_ctx->fileserver = fsmount(renderer_ctx->fileserverfd, nil)) == nil)
		return -1;
		/* sysfatal("fsmount: %r"); */
	return 0;
}


int
clientmount(RendererCtx *renderer_ctx)
{
	if ((renderer_ctx->fileserver = nsmount(renderer_ctx->fileservername, nil)) == nil)
		return -1;
		/* sysfatal("nsmount: %r"); */
	return 0;
}


void
reset_filectx(RendererCtx *renderer_ctx)
{
	LOG("deleting fileserver name ...");
	if (renderer_ctx->filename) {
		free(renderer_ctx->filename);
		renderer_ctx->filename = nil;
	}
	LOG("unmounting fileserver ...");
	if (renderer_ctx->fileserver) {
		fsunmount(renderer_ctx->fileserver);
		renderer_ctx->fileserver = nil;
	}
	LOG("closing the network connection ...");
	if (renderer_ctx->fileserverfid) {
		fsclose(renderer_ctx->fileserverfid);
		renderer_ctx->fileserverfid = nil;
	}
	LOG("closing server file descriptor ...");
	if (renderer_ctx->fileserverfd != -1) {
		close(renderer_ctx->fileserverfd);
		renderer_ctx->fileserverfd = -1;
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
	RendererCtx *renderer_ctx = r->fid->file->aux;
	if (renderer_ctx) {
		LOG("sending command: %d ...", command.cmd);
		send(renderer_ctx->cmdq, &command);
	}
	else {
		LOG("server file has no renderer context");
	}
	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}


int
open_9pconnection(RendererCtx *renderer_ctx)
{
	// FIXME restructure server open/close code
	LOG("opening 9P connection ...");
	int ret;
	if (!renderer_ctx->fileserver) {
		if (renderer_ctx->isaddr) {
			/* renderer_ctx->fileserver = clientdial(renderer_ctx->fileservername); */
			ret = clientdial(renderer_ctx);
		}
		else {
			/* renderer_ctx->fileserver = clientmount(renderer_ctx->fileservername); */
			ret = clientmount(renderer_ctx);
		}
		/* renderer_ctx->fileserver = clientdial("tcp!localhost!5640"); */
		/* renderer_ctx->fileserver = clientdial("tcp!192.168.1.85!5640"); */
		if (ret == -1) {
			LOG("failed to open 9P connection");
			return ret;
		}
	}
	LOG("opening 9P file ...");
	CFid *fid = fsopen(renderer_ctx->fileserver, renderer_ctx->filename, OREAD);
	if (fid == nil) {
		renderer_ctx->renderer_state = RSTATE_STOP;
		blank_window(renderer_ctx);
		return -1;
	}
	renderer_ctx->fileserverfid = fid; 
	return 0;
}


int
read_cmd(RendererCtx *renderer_ctx)
{
	while (renderer_ctx->filename == nil || renderer_ctx->renderer_state == RSTATE_STOP) {
		LOG("renderer stopped or no av stream file specified, waiting for command ...");
		blank_window(renderer_ctx);
		Command cmd;
		int cmdret = recv(renderer_ctx->cmdq, &cmd);
		if (cmdret == 1) {
			LOG("<== received command: %d", cmd.cmd);
			if (cmd.cmd == CMD_SET) {
				/* setstr(&renderer_ctx->filename, cmd.arg, cmd.narg); */
				setstr(&renderer_ctx->url, cmd.arg, cmd.narg);
				seturl(renderer_ctx, renderer_ctx->url);
				free(cmd.arg);
				cmd.arg = nil;
			}
			else if (cmd.cmd == CMD_PLAY) {
				renderer_ctx->renderer_state = RSTATE_PLAY;
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
poll_cmd(RendererCtx *renderer_ctx)
{
	if (renderer_ctx->renderer_state == RSTATE_PLAY) {
		Command cmd;
		int cmdret = nbrecv(renderer_ctx->cmdq, &cmd);
		if (cmdret == 1) {
			LOG("<== received command: %d", cmd.cmd);
			if (cmd.cmd == CMD_PAUSE) {
				renderer_ctx->renderer_state = RSTATE_PAUSE;
				cmdret = recv(renderer_ctx->cmdq, &cmd);
				while (cmdret != 1 || cmd.cmd != CMD_PAUSE) {
					LOG("<== received command: %d", cmd.cmd);
					cmdret = recv(renderer_ctx->cmdq, &cmd);
				}
				renderer_ctx->renderer_state = RSTATE_PLAY;
			}
			else if (cmd.cmd == CMD_STOP) {
				renderer_ctx->renderer_state = RSTATE_STOP;
				reset_filectx(renderer_ctx);
				blank_window(renderer_ctx);
				return -1;
			}
			else if (cmd.cmd == CMD_SEEK) {
				/* renderer_ctx->renderer_state = RSTATE_SEEK; */
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
start_server(RendererCtx *renderer_ctx)
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
	createfile(server.tree->root, "ctl", nil, 0777, renderer_ctx);
	/* f->aux = renderer_ctx; */
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
create_sdl_window(RendererCtx *renderer_ctx)
{
	SDL_DisplayMode DM;
	if (SDL_GetCurrentDisplayMode(0, &DM) != 0) {
		LOG("failed to get sdl display mode");
		return -1;
	}
	renderer_ctx->screen_width  = DM.w;
	renderer_ctx->screen_height = DM.h;
	int requested_window_width  = 800;
	int requested_window_height = 600;
	if (renderer_ctx->sdl_window == nil) {
		// create a window with the specified position, dimensions, and flags.
		renderer_ctx->sdl_window = SDL_CreateWindow(
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
	if (renderer_ctx->sdl_window == nil) {
		LOG("SDL: could not create window");
		return -1;
	}
	if (renderer_ctx->sdl_renderer == nil) {
		// create a 2D rendering context for the SDL_Window
		renderer_ctx->sdl_renderer = SDL_CreateRenderer(
			renderer_ctx->sdl_window,
			-1,
			SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
	}
	return 0;
}


void
get_sdl_window_size(RendererCtx *renderer_ctx)
{
	SDL_GetWindowSize(renderer_ctx->sdl_window, &renderer_ctx->w, &renderer_ctx->h);
}


int
calc_videoscale(RendererCtx *renderer_ctx)
{
	if (renderer_ctx->video_ctx == nil) {
		return -1;
	}
	int w = renderer_ctx->w;
	int h = renderer_ctx->h;
	float war = (float)h / w;
	float far = (float)renderer_ctx->video_ctx->height / renderer_ctx->video_ctx->width;
	int aw = h / far;
	int ah = h;
	if (war > far) {
		aw = w;
		ah = w * far;
	}
	renderer_ctx->aw = aw;
	renderer_ctx->ah = ah;
	LOG("scaling frame: %dx%d to win size: %dx%d, aspect ratio win: %f, aspect ratio frame: %f, final picture size: %dx%d", renderer_ctx->video_ctx->width, renderer_ctx->video_ctx->height, w, h, war, far, aw, ah);
	return 0;
}


int
resize_video(RendererCtx *renderer_ctx)
{
	if (renderer_ctx->video_ctx == nil) {
		LOG("cannot resize video picture, no video context");
		return -1;
	}
	get_sdl_window_size(renderer_ctx);
	calc_videoscale(renderer_ctx);
	renderer_ctx->blit_copy_rect.x = 0.5 * (renderer_ctx->w - renderer_ctx->aw);
	renderer_ctx->blit_copy_rect.y = 0.5 * (renderer_ctx->h - renderer_ctx->ah);
	renderer_ctx->blit_copy_rect.w = renderer_ctx->aw;
	renderer_ctx->blit_copy_rect.h = renderer_ctx->ah;
	LOG("setting scaling context and texture for video frame to size: %dx%d", renderer_ctx->aw, renderer_ctx->ah);
	if (renderer_ctx->yuv_ctx != nil) {
		av_free(renderer_ctx->yuv_ctx);
	}
	renderer_ctx->yuv_ctx = sws_getContext(
		renderer_ctx->video_ctx->width,
		renderer_ctx->video_ctx->height,
		renderer_ctx->video_ctx->pix_fmt,
		// set video size for the ffmpeg image scaler
		renderer_ctx->aw,
		renderer_ctx->ah,
		AV_PIX_FMT_YUV420P,
		SWS_BILINEAR,
		nil,
		nil,
		nil
	);
	if (renderer_ctx->sdl_texture != nil) {
		SDL_DestroyTexture(renderer_ctx->sdl_texture);
	}
	renderer_ctx->sdl_texture = SDL_CreateTexture(
		renderer_ctx->sdl_renderer,
		SDL_PIXELFORMAT_YV12,
		/* SDL_TEXTUREACCESS_STREAMING, */
		SDL_TEXTUREACCESS_TARGET,  // fast update w/o locking, can be used as a render target
		// set video size as the dimensions of the texture
		renderer_ctx->aw,
		renderer_ctx->ah
		);
	if (renderer_ctx->rgb_ctx != nil) {
		av_free(renderer_ctx->rgb_ctx);
	}
	renderer_ctx->rgb_ctx = sws_getContext(
		renderer_ctx->video_ctx->width,
		renderer_ctx->video_ctx->height,
		renderer_ctx->video_ctx->pix_fmt,
		renderer_ctx->video_ctx->width,
		renderer_ctx->video_ctx->height,
		AV_PIX_FMT_RGB24,
		SWS_BILINEAR,
		nil,
		nil,
		nil
	);
	return 0;
}


void
blank_window(RendererCtx *renderer_ctx)
{
	SDL_SetRenderDrawColor(renderer_ctx->sdl_renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
	SDL_RenderClear(renderer_ctx->sdl_renderer);
	SDL_RenderPresent(renderer_ctx->sdl_renderer);
}


int
setup_format_ctx(RendererCtx *renderer_ctx)
{
	LOG("setting up IO context ...");
	unsigned char *avctxBuffer;
	avctxBuffer = malloc(avctxBufferSize);
	AVIOContext *io_ctx = avio_alloc_context(
		avctxBuffer,                   // buffer
		avctxBufferSize,               // buffer size
		0,                             // buffer is only readable - set to 1 for read/write
		renderer_ctx->fileserverfid,   // user specified data
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
	renderer_ctx->io_ctx = io_ctx;
	renderer_ctx->format_ctx = format_ctx;
	return 0;
}


int
open_input_stream(RendererCtx *renderer_ctx)
{
	LOG("opening input stream ...");
	int ret = avformat_open_input(&renderer_ctx->format_ctx, nil, nil, nil);
	if (ret < 0) {
		LOG("Could not open file %s", renderer_ctx->filename);
		if (renderer_ctx->io_ctx) {
			avio_context_free(&renderer_ctx->io_ctx);
		}
		avformat_close_input(&renderer_ctx->format_ctx);
		if (renderer_ctx->format_ctx) {
			avformat_free_context(renderer_ctx->format_ctx);
		}
		return -1;
	}
	LOG("opened input stream");
	return 0;
}


int
open_stream_component(RendererCtx *renderer_ctx, int stream_index)
{
	LOG("opening stream component ...");
	AVFormatContext *format_ctx = renderer_ctx->format_ctx;
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
			codecCtx->sample_rate, renderer_ctx->audio_out_channels);
		SDL_AudioSpec wanted_specs;
		wanted_specs.freq = codecCtx->sample_rate;
		wanted_specs.format = AUDIO_S16SYS;
		wanted_specs.channels = renderer_ctx->audio_out_channels;
		wanted_specs.silence = 0;
		wanted_specs.samples = SDL_AUDIO_BUFFER_SIZE;
		wanted_specs.callback = nil;
		wanted_specs.userdata = renderer_ctx;
		renderer_ctx->audio_devid = SDL_OpenAudioDevice(nil, 0, &wanted_specs, &renderer_ctx->specs, 0);
		if (renderer_ctx->audio_devid == 0) {
			printf("SDL_OpenAudio: %s.\n", SDL_GetError());
			return -1;
		}
		LOG("audio device with id: %d opened successfully", renderer_ctx->audio_devid);
		LOG("audio specs are freq: %d, channels: %d", renderer_ctx->specs.freq, renderer_ctx->specs.channels);
	}
	if (avcodec_open2(codecCtx, codec, nil) < 0) {
		LOG("Unsupported codec");
		return -1;
	}
	switch (codecCtx->codec_type) {
		case AVMEDIA_TYPE_AUDIO:
		{
			LOG("setting up audio stream context ...");
			renderer_ctx->audio_stream = stream_index;
			renderer_ctx->audio_ctx = codecCtx;
			renderer_ctx->audio_buf_size = 0;
			renderer_ctx->audio_buf_index = 0;
			memset(&renderer_ctx->audio_pkt, 0, sizeof(renderer_ctx->audio_pkt));
			/* renderer_ctx->audioq = chancreate(sizeof(AVPacket*), MAX_AUDIOQ_SIZE); */
			renderer_ctx->audioq = chancreate(sizeof(AVPacket), MAX_AUDIOQ_SIZE);
			renderer_ctx->presenter_tid = threadcreate(presenter_thread, renderer_ctx, THREAD_STACK_SIZE);
			LOG("presenter thread created with id: %i", renderer_ctx->presenter_tid);
			LOG("calling sdl_pauseaudio(0) ...");
			/* SDL_PauseAudio(0); */
			SDL_PauseAudioDevice(renderer_ctx->audio_devid, 0);
			LOG("sdl_pauseaudio(0) called.");
			renderer_ctx->audio_timebase = renderer_ctx->format_ctx->streams[stream_index]->time_base;
			LOG("timebase of audio stream: %d/%d",
				renderer_ctx->audio_timebase.num, renderer_ctx->audio_timebase.den);
		}
			break;
		case AVMEDIA_TYPE_VIDEO:
		{
			LOG("setting up video stream context ...");
			renderer_ctx->video_stream = stream_index;
			renderer_ctx->video_ctx = codecCtx;
			renderer_ctx->pictq = chancreate(sizeof(VideoPicture), VIDEO_PICTURE_QUEUE_SIZE);
			resize_video(renderer_ctx);
			renderer_ctx->video_timebase = renderer_ctx->format_ctx->streams[stream_index]->time_base;
			LOG("timebase of video stream: %d/%d",
				renderer_ctx->video_timebase.num, renderer_ctx->video_timebase.den);
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
open_stream_components(RendererCtx *renderer_ctx)
{
	int ret = avformat_find_stream_info(renderer_ctx->format_ctx, nil);
	if (ret < 0) {
		LOG("Could not find stream information: %s.", renderer_ctx->filename);
		return -1;
	}
	if (_DEBUG_) {
		av_dump_format(renderer_ctx->format_ctx, 0, renderer_ctx->filename, 0);
	}
	int video_stream = -1;
	int audio_stream = -1;
	for (int i = 0; i < renderer_ctx->format_ctx->nb_streams; i++)
	{
		if (renderer_ctx->format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream < 0) {
			video_stream = i;
		}
		if (renderer_ctx->format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream < 0) {
			audio_stream = i;
		}
	}
	if (video_stream == -1) {
		LOG("Could not find video stream.");
	}
	else {
		ret = open_stream_component(renderer_ctx, video_stream);
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
		ret = open_stream_component(renderer_ctx, audio_stream);
		// check audio codec was opened correctly
		if (ret < 0) {
			LOG("Could not open audio codec.");
			return -1;
		}
		LOG("audio stream component opened successfully.");
	}
	if (renderer_ctx->video_stream < 0 && renderer_ctx->audio_stream < 0) {
		LOG("both video and audio stream missing");
		return -1;
	}
	return 0;
}


int
alloc_buffers(RendererCtx *renderer_ctx)
{
	if (renderer_ctx->video_ctx) {
		// yuv buffer for displaying to screen
	    int yuv_num_bytes = av_image_get_buffer_size(
			AV_PIX_FMT_YUV420P,
			renderer_ctx->w,
			renderer_ctx->h,
			/* renderer_ctx->aw, */
			/* renderer_ctx->ah, */
			// crash on bunny with buffer size below
			/* renderer_ctx->video_ctx->width, */
			/* renderer_ctx->video_ctx->height, */
			32
			);
		renderer_ctx->yuvbuffer = (uint8_t *) av_malloc(yuv_num_bytes * sizeof(uint8_t));
		// rgb buffer for saving to disc
	    renderer_ctx->frame_rgb = av_frame_alloc();
	    int rgb_num_bytes;
	    rgb_num_bytes = av_image_get_buffer_size(
			AV_PIX_FMT_RGB24,
			renderer_ctx->video_ctx->width,
			renderer_ctx->video_ctx->height,
			32
			);
	    renderer_ctx->rgbbuffer = (uint8_t *) av_malloc(rgb_num_bytes * sizeof(uint8_t));
	    av_image_fill_arrays(
			renderer_ctx->frame_rgb->data,
			renderer_ctx->frame_rgb->linesize,
			renderer_ctx->rgbbuffer,
			AV_PIX_FMT_RGB24,
			renderer_ctx->video_ctx->width,
			renderer_ctx->video_ctx->height,
			32
	    );
	}
	return 0;
}


int
read_packet(RendererCtx *renderer_ctx, AVPacket* packet)
{
	int demuxer_ret = av_read_frame(renderer_ctx->format_ctx, packet);
	LOG("read av packet of size: %i", packet->size);
	if (demuxer_ret < 0) {
		LOG("failed to read av packet: %s", av_err2str(demuxer_ret));
		if (demuxer_ret == AVERROR_EOF) {
			LOG("EOF");
		}
		renderer_ctx->renderer_state = RSTATE_STOP;
		reset_filectx(renderer_ctx);
		blank_window(renderer_ctx);
		return -1;
	}
	if (packet->size == 0) {
		LOG("packet size is zero, exiting demuxer thread");
		return -1;
	}
	return 0;
}


int
write_packet_to_decoder(RendererCtx *renderer_ctx, AVPacket* packet)
{
	AVCodecContext *codecCtx = nil;
	if (packet->stream_index == renderer_ctx->video_stream) {
		LOG("sending video packet of size %d to decoder", packet->size);
		codecCtx = renderer_ctx->video_ctx;
	}
	else {
		LOG("sending audio packet of size %d to decoder", packet->size);
		codecCtx = renderer_ctx->audio_ctx;
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
		renderer_ctx->renderer_state = RSTATE_STOP;
		reset_filectx(renderer_ctx);
		blank_window(renderer_ctx);
	}
	if (decsend_ret < 0) {
		LOG("error sending packet to decoder: %s", av_err2str(decsend_ret));
		return -1;
	}
	renderer_ctx->current_codec_ctx = codecCtx;
	return 0;
}


int
read_frame_from_decoder(RendererCtx *renderer_ctx, AVFrame *pFrame)
{
	LOG("reading decoded frame from decoder ...");
	int ret = avcodec_receive_frame(renderer_ctx->current_codec_ctx, pFrame);
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
create_pristine_picture_from_frame(RendererCtx *renderer_ctx, AVFrame *pFrame, VideoPicture *videoPicture)
{
	// FIXME sending pristine frames over the video picture channel doesn't work
	videoPicture->pix_fmt = renderer_ctx->current_codec_ctx->pix_fmt;
	LOG("AV_NUM_DATA_POINTERS: %d", AV_NUM_DATA_POINTERS);
	/* memcpy(videoPicture->linesizes, pFrame->linesize, 4 * sizeof(uint8_t*) / 8); */
	/* memcpy(videoPicture->linesizes, pFrame->linesize, AV_NUM_DATA_POINTERS); */
	/* memcpy(videoPicture->linesizes, pFrame->linesize, AV_NUM_DATA_POINTERS * sizeof(uint8_t*)); */
	/* memcpy(videoPicture->linesizes, pFrame->linesize, AV_NUM_DATA_POINTERS * sizeof(uint8_t*) / 8); */
	/* memcpy(videoPicture->planes, pFrame->data, AV_NUM_DATA_POINTERS * sizeof(uint8_t*)); */
	/* memcpy(videoPicture->planes, pFrame->data, 4 * sizeof(uint8_t*) / 8); */
	// FIXME avoid hardcoding parameter align to 32 ...
	LOG("allocating video picture for queueing ...");
	int frame_size = av_image_alloc(
		videoPicture->planes,
		pFrame->linesize,
		renderer_ctx->current_codec_ctx->width,
		renderer_ctx->current_codec_ctx->height,
		renderer_ctx->current_codec_ctx->pix_fmt,
		32
	);
	USED(frame_size);
	LOG("copying video picture for queueing ...");
	av_image_copy(
		videoPicture->planes,
		//videoPicture->linesizes,
	          pFrame->linesize,
	          (uint8_t const**)pFrame->data,
	          pFrame->linesize,
		renderer_ctx->current_codec_ctx->pix_fmt,
		renderer_ctx->current_codec_ctx->width,
		renderer_ctx->current_codec_ctx->height
	);
	/* av_frame_copy(); */
	return 0;
}


int
create_rgb_picture_from_frame(RendererCtx *renderer_ctx, AVFrame *pFrame, VideoPicture *videoPicture)
{
	sws_scale(
	    renderer_ctx->rgb_ctx,
	    (uint8_t const * const *)pFrame->data,
	    pFrame->linesize,
	    0,
	    renderer_ctx->current_codec_ctx->height,
	    renderer_ctx->frame_rgb->data,
	    renderer_ctx->frame_rgb->linesize
	);
	// av_frame_unref(pFrame);
	videoPicture->linesize = renderer_ctx->frame_rgb->linesize[0];
	int rgb_num_bytes = av_image_get_buffer_size(
		AV_PIX_FMT_RGB24,
		renderer_ctx->current_codec_ctx->width,
		renderer_ctx->current_codec_ctx->height,
		32
		);
	videoPicture->rgbbuf = (uint8_t *) av_malloc(rgb_num_bytes * sizeof(uint8_t));
	memcpy(videoPicture->rgbbuf, renderer_ctx->frame_rgb->data[0], rgb_num_bytes);
	return 0;
}


int
create_yuv_picture_from_frame(RendererCtx *renderer_ctx, AVFrame *pFrame, VideoPicture *videoPicture)
{
	LOG("scaling video picture (height %d) to target size %dx%d before queueing",
		renderer_ctx->current_codec_ctx->height, renderer_ctx->aw, renderer_ctx->ah);
	videoPicture->frame = av_frame_alloc();
	av_image_fill_arrays(
			videoPicture->frame->data,
			videoPicture->frame->linesize,
			renderer_ctx->yuvbuffer,
			AV_PIX_FMT_YUV420P,
			// set video size of picture to send to queue here
			renderer_ctx->aw, 
			renderer_ctx->ah,
			32
	);
	sws_scale(
	    renderer_ctx->yuv_ctx,
	    (uint8_t const * const *)pFrame->data,
	    pFrame->linesize,
	    0,
	    // set video height here to select the slice in the *SOURCE* picture to scale (usually the whole picture)
	    renderer_ctx->current_codec_ctx->height,
	    videoPicture->frame->data,
	    videoPicture->frame->linesize
	);
	LOG("video picture created.");
	return 0;
}


int
create_sample_from_frame(RendererCtx *renderer_ctx, AVFrame *pFrame, AudioSample *audioSample)
{
	int bytes_per_sec = 2 * renderer_ctx->current_codec_ctx->sample_rate * renderer_ctx->audio_out_channels;
	int data_size = resample_audio(
			renderer_ctx,
			pFrame,
			AV_SAMPLE_FMT_S16,
			renderer_ctx->audio_buf);
	double sample_duration = 1000.0 * data_size / bytes_per_sec;
	audioSample->size = data_size;
	audioSample->duration = sample_duration;
	LOG("resampled audio bytes: %d", data_size);
	memcpy(audioSample->sample, renderer_ctx->audio_buf, sizeof(renderer_ctx->audio_buf));
	/* double sample_duration = 0.5 * 1000.0 * audioSample->size / bytes_per_sec; */
	/* double sample_duration = 2 * 1000.0 * audioSample->size / bytes_per_sec; */
	LOG("audio sample rate: %d, channels: %d, duration: %.2fms",
		renderer_ctx->current_codec_ctx->sample_rate, renderer_ctx->audio_out_channels, audioSample->duration);
	/* renderer_ctx->audio_pts += sample_duration;  // audio sample length in ms */
	return 0;
}


void
send_picture_to_queue(RendererCtx *renderer_ctx, VideoPicture *videoPicture)
{
	LOG("==> sending picture with idx: %d, pts: %.2fms to picture queue ...", videoPicture->idx, videoPicture->pts);
	int sendret = send(renderer_ctx->pictq, videoPicture);
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
send_sample_to_queue(RendererCtx *renderer_ctx, AudioSample *audioSample)
{
	int sendret = send(renderer_ctx->audioq, audioSample);
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
	RendererCtx *renderer_ctx = (RendererCtx *)arg;
	LOG("decoder thread started with id: %d", renderer_ctx->decoder_tid);

start:
	if (read_cmd(renderer_ctx) == 1) {
		goto quit;
	}
	if (open_9pconnection(renderer_ctx) == -1) {
		goto start;
	}
	if (setup_format_ctx(renderer_ctx) == -1) {
		goto start;
	}
	if (open_input_stream(renderer_ctx) == -1) {
		goto start;
	}
	if (open_stream_components(renderer_ctx) == -1) {
		goto start;
	}
	if (alloc_buffers(renderer_ctx) == -1) {
		goto start;
	}

	AVPacket *packet = av_packet_alloc();
	if (packet == nil) {
		LOG("Could not allocate AVPacket.");
		goto start;
	}
	AVFrame *pFrame = av_frame_alloc();
	if (pFrame == nil) {
		printf("Could not allocate AVFrame.\n");
		goto start;
	}
	double audio_pts = 0.0;
	double video_pts = 0.0;

	// Main decoder loop
	for (;;) {
		int jmp = poll_cmd(renderer_ctx);
		if (jmp == -1) {
			goto start;
		}
		if (jmp == 1) {
			goto quit;
		}
		if (read_packet(renderer_ctx, packet) == -1) {
			goto start;
		}
		if (write_packet_to_decoder(renderer_ctx, packet) == -1) {
			goto start;
		}
		// This loop is only needed when we get more than one decoded frame out
		// of one packet read from the demuxer
		int decoder_ret = 0;
		while (decoder_ret == 0) {
			decoder_ret = read_frame_from_decoder(renderer_ctx, pFrame);
			if (decoder_ret == -1) {
				goto start;
			}
			if (decoder_ret == 2) {
				break;
			}
			// TODO it would be nicer to check for the frame type instead for the codec context
			if (renderer_ctx->current_codec_ctx == renderer_ctx->video_ctx) {
				renderer_ctx->video_idx++;
				renderer_ctx->frame_rate = av_q2d(renderer_ctx->video_ctx->framerate);
				renderer_ctx->frame_duration = 1000.0 / renderer_ctx->frame_rate;
				LOG("video frame duration: %.2fms, fps: %.2f",
					renderer_ctx->frame_duration, 1000.0 / renderer_ctx->frame_duration);
				video_pts += renderer_ctx->frame_duration;
				VideoPicture videoPicture = {
					.frame = nil,
					.rgbbuf = nil,
					.planes = nil,
					.width = renderer_ctx->aw,
					.height = renderer_ctx->ah,
					.idx = renderer_ctx->video_idx,
					.pts = video_pts,
					};
				if (renderer_ctx->frame_fmt == FRAME_FMT_PRISTINE) {
					if (create_pristine_picture_from_frame(renderer_ctx, pFrame, &videoPicture) == 2) {
						break;
					}
				}
				else if (renderer_ctx->frame_fmt == FRAME_FMT_RGB) {
					if (create_rgb_picture_from_frame(renderer_ctx, pFrame, &videoPicture) == 2) {
						break;
					}
			    }
				else if (renderer_ctx->frame_fmt == FRAME_FMT_YUV) {
					if (create_yuv_picture_from_frame(renderer_ctx, pFrame, &videoPicture) == 2) {
						break;
					}
			    }
				if (!renderer_ctx->audio_only) {
					send_picture_to_queue(renderer_ctx, &videoPicture);
				}
			}
			else if (renderer_ctx->current_codec_ctx == renderer_ctx->audio_ctx) {
				renderer_ctx->audio_idx++;
				AudioSample audioSample = {
					.idx = renderer_ctx->audio_idx,
					.sample = malloc(sizeof(renderer_ctx->audio_buf)),
					};
				if (create_sample_from_frame(renderer_ctx, pFrame, &audioSample) == 2) {
					break;
				}
				audio_pts += audioSample.duration;
				audioSample.pts = audio_pts;
				send_sample_to_queue(renderer_ctx, &audioSample);
			}
			else {
				LOG("non AV packet from demuxer, ignoring");
			}
		}
		av_packet_unref(packet);
		av_frame_unref(pFrame);
	}

quit:
	renderer_ctx->renderer_state = RSTATE_QUIT;
	// Clean up the decoder thread
	if (renderer_ctx->io_ctx) {
		avio_context_free(&renderer_ctx->io_ctx);
	}
	avformat_close_input(&renderer_ctx->format_ctx);
	if (renderer_ctx->format_ctx) {
		avformat_free_context(renderer_ctx->format_ctx);
	}
	/* if (renderer_ctx->videoq) { */
		/* chanfree(renderer_ctx->videoq); */
	/* } */
	if (renderer_ctx->audioq) {
		chanfree(renderer_ctx->audioq);
	}
	if (renderer_ctx->pictq) {
		chanfree(renderer_ctx->pictq);
	}
	fclose(audio_out);
	// in case of failure, push the FF_QUIT_EVENT and return
	LOG("quitting decoder thread");
	threadexitsall("end of file");
}


void
receive_picture(RendererCtx *renderer_ctx, VideoPicture *videoPicture)
{
	LOG("receiving picture from picture queue ...");
	int recret = recv(renderer_ctx->pictq, videoPicture);
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
	RendererCtx *renderer_ctx = arg;
	AudioSample audioSample;
	VideoPicture videoPicture;
	renderer_ctx->audio_start_rt = av_gettime();
	renderer_ctx->current_video_time = renderer_ctx->audio_start_rt;
	renderer_ctx->previous_video_time = renderer_ctx->audio_start_rt;

	if (renderer_ctx->video_ctx) {
		receive_picture(renderer_ctx, &videoPicture);
	}
	for (;;) {
		LOG("receiving sample from audio queue ...");
		int recret = recv(renderer_ctx->audioq, &audioSample);
		if (recret != 1) {
			LOG("<== error when receiving sample from audio queue");
			continue;
		}
		LOG("<== received sample with idx: %d, pts: %.2fms from audio queue.", audioSample.idx, audioSample.pts);
		//fwrite(audioSample.sample, 1, audioSample.size, audio_out);
		int ret = SDL_QueueAudio(renderer_ctx->audio_devid, audioSample.sample, audioSample.size);
		if (ret < 0) {
			LOG("failed to write audio sample: %s", SDL_GetError());
			free(audioSample.sample);
			continue;
		}
		LOG("queued audio sample to sdl device");

		int audioq_size = SDL_GetQueuedAudioSize(renderer_ctx->audio_devid);
		int bytes_per_sec = 2 * renderer_ctx->audio_ctx->sample_rate * renderer_ctx->audio_out_channels;
		double queue_duration = 1000.0 * audioq_size / bytes_per_sec;
		int samples_queued = audioq_size / audioSample.size;
		// current_audio_pts = audioSample.pts - queue_duration;
		double real_time = (av_gettime() - renderer_ctx->audio_start_rt) / 1000.0;
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

		if (renderer_ctx->video_ctx &&
			fabs(audioSample.pts - videoPicture.pts) <= 0.5 * audioSample.duration)
		{
			receive_picture(renderer_ctx, &videoPicture);
			display_picture(renderer_ctx, &videoPicture);
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
display_picture(RendererCtx *renderer_ctx, VideoPicture *videoPicture)
{
	if (!videoPicture->frame) {
		LOG("no picture to display");
		return;
	}
	double real_time = (av_gettime() - renderer_ctx->audio_start_rt) / 1000.0;
	renderer_ctx->previous_video_time = renderer_ctx->current_video_time;
	renderer_ctx->current_video_time = real_time;
	LOG("displaying picture %d, delta time: %.2fms ...",
		videoPicture->idx,
		renderer_ctx->current_video_time - renderer_ctx->previous_video_time - renderer_ctx->frame_duration);
	int textupd = SDL_UpdateYUVTexture(
			renderer_ctx->sdl_texture,
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
	SDL_SetRenderDrawColor(renderer_ctx->sdl_renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);  // FIXME needed ...?
	SDL_RenderClear(renderer_ctx->sdl_renderer);
	// copy and place a portion of the texture to the current rendering target
	// set video size when copying sdl texture to sdl renderer. Texture will be stretched to blit_copy_rect!
	SDL_RenderCopy(renderer_ctx->sdl_renderer, renderer_ctx->sdl_texture, nil, &renderer_ctx->blit_copy_rect);
	// update the screen with any rendering performed since the previous call
	SDL_RenderPresent(renderer_ctx->sdl_renderer);
}


int 
resample_audio(RendererCtx *renderer_ctx, AVFrame *decoded_audio_frame, enum AVSampleFormat out_sample_fmt, uint8_t *out_buf)
{
	// get an instance of the AudioResamplingState struct
	AudioResamplingState * arState = getAudioResampling(renderer_ctx->audio_ctx->channel_layout);
	if (!arState->swr_ctx) {
		printf("swr_alloc error.\n");
		return -1;
	}
	// get input audio channels
	arState->in_channel_layout = (renderer_ctx->audio_ctx->channels ==
								  av_get_channel_layout_nb_channels(renderer_ctx->audio_ctx->channel_layout)) ?
								 renderer_ctx->audio_ctx->channel_layout :
								 av_get_default_channel_layout(renderer_ctx->audio_ctx->channels);
	// check input audio channels correctly retrieved
	if (arState->in_channel_layout <= 0) {
		printf("in_channel_layout error.\n");
		return -1;
	}
	// set output audio channels based on the input audio channels
	if (renderer_ctx->audio_ctx->channels == 1) {
		arState->out_channel_layout = AV_CH_LAYOUT_MONO;
	}
	else if (renderer_ctx->audio_ctx->channels == 2) {
		arState->out_channel_layout = AV_CH_LAYOUT_STEREO;
	}
	else {
		arState->out_channel_layout = AV_CH_LAYOUT_SURROUND;
	}
	// retrieve number of audio samples (per channel)
	arState->in_nb_samples = decoded_audio_frame->nb_samples;
	if (arState->in_nb_samples <= 0) {
		printf("in_nb_samples error.\n");
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
			renderer_ctx->audio_ctx->sample_rate,
			0
	);
	// Set SwrContext parameters for resampling
	av_opt_set_sample_fmt(
			arState->swr_ctx,
			"in_sample_fmt",
			renderer_ctx->audio_ctx->sample_fmt,
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
			renderer_ctx->audio_ctx->sample_rate,
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
	int ret = swr_init(arState->swr_ctx);;
	if (ret < 0) {
		printf("Failed to initialize the resampling context.\n");
		return -1;
	}
	arState->max_out_nb_samples = arState->out_nb_samples = av_rescale_rnd(
			arState->in_nb_samples,
			renderer_ctx->audio_ctx->sample_rate,
			renderer_ctx->audio_ctx->sample_rate,
			AV_ROUND_UP
	);
	// check rescaling was successful
	if (arState->max_out_nb_samples <= 0) {
		printf("av_rescale_rnd error.\n");
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
		printf("av_samples_alloc_array_and_samples() error: Could not allocate destination samples.\n");
		return -1;
	}
	// retrieve output samples number taking into account the progressive delay
	arState->out_nb_samples = av_rescale_rnd(
			swr_get_delay(arState->swr_ctx, renderer_ctx->audio_ctx->sample_rate) + arState->in_nb_samples,
			renderer_ctx->audio_ctx->sample_rate,
			renderer_ctx->audio_ctx->sample_rate,
			AV_ROUND_UP
	);
	// check output samples number was correctly rescaled
	if (arState->out_nb_samples <= 0) {
		printf("av_rescale_rnd error\n");
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
			printf("av_samples_alloc failed.\n");
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
			printf("swr_convert_error.\n");
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
			printf("av_samples_get_buffer_size error.\n");
			return -1;
		}
	}
	else {
		printf("swr_ctx null error.\n");
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


AudioResamplingState*
getAudioResampling(uint64_t channel_layout)
{
	AudioResamplingState * audioResampling = av_mallocz(sizeof(AudioResamplingState));
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


void
savePicture(RendererCtx* renderer_ctx, VideoPicture *videoPicture, int frameIndex)
{
	AVFrame *frame_rgb = nil;
	uint8_t *buffer = nil;
	if (renderer_ctx->frame_fmt == FRAME_FMT_PRISTINE) {
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
	if (renderer_ctx->frame_fmt == FRAME_FMT_PRISTINE) {
	    for (y = 0; y < frame_rgb->height; y++) {
	        fwrite(frame_rgb->data[0] + y * frame_rgb->linesize[0], 1, videoPicture->width * 3, pFile);
	    }
	}
	else if (renderer_ctx->frame_fmt == FRAME_FMT_RGB) {
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
reset_renderer_ctx(RendererCtx *renderer_ctx)
{
	renderer_ctx->url = nil;
	renderer_ctx->fileservername = nil;
	renderer_ctx->filename = nil;
	renderer_ctx->isaddr = 0;
	renderer_ctx->fileserverfd = -1;
	renderer_ctx->fileserverfid = nil;
	renderer_ctx->fileserver = nil;
	renderer_ctx->io_ctx = nil;
	renderer_ctx->format_ctx = nil;
	renderer_ctx->current_codec_ctx = nil;
	/* renderer_ctx->av_sync_type = DEFAULT_AV_SYNC_TYPE; */
	renderer_ctx->renderer_state = RSTATE_STOP;
	renderer_ctx->cmdq = nil;
	renderer_ctx->screen_width = 0;
	renderer_ctx->screen_height = 0;
	renderer_ctx->window_width = 0;
	renderer_ctx->window_height = 0;
	renderer_ctx->server_tid = 0;
	renderer_ctx->decoder_tid = 0;
	renderer_ctx->presenter_tid = 0;
	renderer_ctx->audio_stream = -1;
	renderer_ctx->audio_ctx = nil;
	renderer_ctx->audioq = nil;
	renderer_ctx->audio_buf_size = 0;
	renderer_ctx->audio_buf_index = 0;
	renderer_ctx->audio_idx = 0;
	renderer_ctx->audio_out_channels = 2;
	renderer_ctx->current_video_time = 0.0;
	renderer_ctx->previous_video_time = 0.0;
	renderer_ctx->audio_start_rt = 0;
	renderer_ctx->audio_timebase.num = 0;
	renderer_ctx->audio_timebase.den = 0;
	// Video Stream.
	renderer_ctx->sdl_window = nil;
	renderer_ctx->sdl_texture = nil;
	renderer_ctx->sdl_renderer = nil;
	renderer_ctx->video_stream = -1;
	renderer_ctx->frame_rate = 0.0;
	renderer_ctx->frame_duration = 0.0;
	renderer_ctx->video_ctx = nil;
	renderer_ctx->pictq = nil;
	renderer_ctx->yuv_ctx = nil;
	renderer_ctx->rgb_ctx = nil;
	renderer_ctx->video_idx = 0;
	renderer_ctx->frame_rgb = nil;
	renderer_ctx->rgbbuffer = nil;
	renderer_ctx->yuvbuffer = nil;
	renderer_ctx->w = 0;
	renderer_ctx->h = 0;
	renderer_ctx->aw = 0;
	renderer_ctx->ah = 0;
	renderer_ctx->video_timebase.num = 0;
	renderer_ctx->video_timebase.den = 0;
	// Seeking
	renderer_ctx->seek_req = 0;
	renderer_ctx->seek_flags = 0;
	renderer_ctx->seek_pos = 0;
	/* renderer_ctx->frame_fmt = FRAME_FMT_RGB; */
	renderer_ctx->frame_fmt = FRAME_FMT_YUV;
	renderer_ctx->audio_devid = -1;
	renderer_ctx->audio_only = 0;
}


void
threadmain(int argc, char **argv)
{
	if (_DEBUG_) {
		chatty9pclient = 1;
		/* chattyfuse = 1; */
	}
	RendererCtx *renderer_ctx = av_mallocz(sizeof(RendererCtx));
	reset_renderer_ctx(renderer_ctx);
	start_server(renderer_ctx);
	/* int ret = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER); */
	int ret = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
	if (ret != 0) {
		LOG("Could not initialize SDL - %s", SDL_GetError());
		return;
	}
	if (create_sdl_window(renderer_ctx) == -1) {
		return;
	}
	/* setstr(&renderer_ctx->fileservername, DEFAULT_SERVER_NAME, 0); */
	blank_window(renderer_ctx);
	renderer_ctx->cmdq = chancreate(sizeof(Command), MAX_COMMANDQ_SIZE);
	if (argc >= 2) {
		seturl(renderer_ctx, argv[1]);
	}
	audio_out = fopen("/tmp/out.pcm", "wb");
	// start the decoding thread to read data from the AVFormatContext
	renderer_ctx->decoder_tid = threadcreate(decoder_thread, renderer_ctx, THREAD_STACK_SIZE);
	if (!renderer_ctx->decoder_tid) {
		printf("could not start decoder thread: %s.\n", SDL_GetError());
		av_free(renderer_ctx);
		return;
	}
	/* av_init_packet(&flush_pkt); */
	/* flush_pkt.data = (uint8_t*)"FLUSH"; */
	for (;;) {
		yield();
		if (renderer_ctx->renderer_state == RSTATE_STOP || renderer_ctx->renderer_state == RSTATE_PAUSE) {
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
						send(renderer_ctx->cmdq, &cmd);
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
							resize_video(renderer_ctx);
						}
						break;
						case SDL_WINDOWEVENT_SHOWN:
						case SDL_WINDOWEVENT_RESTORED:
						{
							LOG("window restored");
							blank_window(renderer_ctx);
						}
					}
				}
				break;
				case SDL_QUIT:
				{
					cmd.cmd = CMD_QUIT;
					send(renderer_ctx->cmdq, &cmd);
				}
				break;
			}
		}
	}
	// FIXME never reached ... need to shut down the renderer properly
	LOG("freeing video state");
	av_free(renderer_ctx);
	return;
}


////////////////////////////////////////////////---////////////////////////////////////////////////


/* void */
/* video_thread(void *arg) */
/* { */
	/* RendererCtx *renderer_ctx = arg; */
	/* VideoPicture videoPicture; */
	/* for (;;) { */
		/* receive_picture(renderer_ctx, &videoPicture); */
		/* if (renderer_ctx->frame_fmt == FRAME_FMT_RGB) { */
			/* savePicture(renderer_ctx, &videoPicture, (int)videoPicture.pts); */
			/* if (videoPicture.rgbbuf) { */
				/* av_free(videoPicture.rgbbuf); */
			/* } */
		/* } */
		/* else if (renderer_ctx->frame_fmt == FRAME_FMT_YUV) { */
			/* LOG("picture with idx: %d, pts: %f, current audio pts: %f", videoPicture.idx, videoPicture.pts, renderer_ctx->current_audio_pts); */
			/* if (renderer_ctx->current_audio_pts >= videoPicture.pts) { */
				/* display_picture(renderer_ctx, &videoPicture); */
			/* } */
			/* else { */
				/* LOG("yielding video_thread"); */
				/* yield(); */
			/* } */
		/* } */
		/* if (videoPicture.frame) { */
			/* av_frame_unref(videoPicture.frame); */
			/* av_frame_free(&videoPicture.frame); */
		/* } */
	/* } */
/* } */


// FIXME need to store the renderer context in a global static variable for now to access
//       it from the timer notification handler
/* static RendererCtx *rctx = nil; */

/* int */
/* handle_display_picture_timer(void *plan9_internal, char *notification) */
/* { */
	/* LOG("received notification '%s'", notification); */
	/* if (strcmp(notification, "alarm") != 0) { */
		/* return 0; */
	/* } */
	/* //RendererCtx *renderer_ctx = rctx; */
	/* //VideoPicture videoPicture; */
	/* //receive_picture(renderer_ctx, &videoPicture); */
	/* //display_picture(renderer_ctx, &videoPicture); */
	/* //if (videoPicture.frame) { */
	/* //	av_frame_unref(videoPicture.frame); */
	/* //	av_frame_free(&videoPicture.frame); */
	/* //} */
	/* long time_left = alarm(33); */
	/* LOG("alarm set, time left: %ld", time_left); */
	/* yield(); */
	/* return 1; */
/* } */


/* void */
/* grab_and_display_picture(RendererCtx *renderer_ctx) */
/* { */
	/* VideoPicture videoPicture; */
	/* receive_picture(renderer_ctx, &videoPicture); */
	/* display_picture(renderer_ctx, &videoPicture); */
	/* if (videoPicture.frame) { */
		/* av_frame_unref(videoPicture.frame); */
		/* av_frame_free(&videoPicture.frame); */
	/* } */
/* } */


/* void */
/* audio_thread(void *arg) */
/* { */
	/* RendererCtx *renderer_ctx = arg; */
	/* VideoPicture videoPicture; */
	/* renderer_ctx->current_video_pts = 0; */
	/* if (renderer_ctx->video_ctx) { */
		/* receive_picture(renderer_ctx, &videoPicture); */
	/* } */
	/* renderer_ctx->current_video_time = renderer_ctx->audio_start_rt; */
	/* renderer_ctx->previous_video_time = renderer_ctx->audio_start_rt; */
	/* renderer_ctx->audio_start_rt = av_gettime(); */
	/* for (;;) { */
		/* LOG("receiving sample from audio queue ..."); */
		/* AudioSample audioSample; */
		/* int recret = recv(renderer_ctx->audioq, &audioSample); */
		/* if (recret != 1) { */
			/* LOG("<== error when receiving sample from audio queue"); */
			/* continue; */
		/* } */
		/* LOG("<== received sample with idx: %d, pts: %f from audio queue.", audioSample.idx, audioSample.pts); */

		/* //if (samples_queued < 5) { */
		/* //	yield(); */
		/* //	LOG("sleeping"); */
		/* //	sleep(sample_duration); */
		/* //} */

		/* //fwrite(audioSample.sample, 1, audioSample.size, audio_out); */
		/* int ret = SDL_QueueAudio(renderer_ctx->audio_devid, audioSample.sample, audioSample.size); */
		/* if (ret < 0) { */
			/* LOG("failed to write audio sample: %s", SDL_GetError()); */
			/* free(audioSample.sample); */
			/* continue; */
		/* } */
		/* int audioq_size = SDL_GetQueuedAudioSize(renderer_ctx->audio_devid); */

		/* //int bytes_per_sec = 2 * renderer_ctx->audio_ctx->sample_rate * renderer_ctx->audio_ctx->channels; */
		/* int bytes_per_sec = 2 * renderer_ctx->audio_ctx->sample_rate * 2; */

		/* double queue_duration = 1000.0 * audioq_size / bytes_per_sec; */
		/* int samples_queued = audioq_size / audioSample.size; */
		/* double sample_duration = 1000.0 * audioSample.size / bytes_per_sec; */
		/* // renderer_ctx->current_audio_pts = audioSample.pts - queue_duration; */
		/* renderer_ctx->current_audio_pts = audioSample.pts; */
		/* double real_time = (av_gettime() - renderer_ctx->audio_start_rt) / 1000.0; */
		/* double time_diff = renderer_ctx->current_audio_pts - real_time; */
		/* LOG("audio sample idx: %d, size: %d", audioSample.idx, audioSample.size); */
		/* LOG("audio clock: %f, real time: %f, current video pts %f", */
		/* renderer_ctx->current_audio_pts, real_time, renderer_ctx->current_video_pts); */
		/* LOG("sdl audio queue size in bytes: %d, msec: %f, samples: %d", */
			/* audioq_size, queue_duration, samples_queued); */

		/* //if (!rctx) { */
		/* //	// Install timer notification handler */
		/* //	LOG("installing timer notification handler"); */
		/* //	rctx = renderer_ctx; */
		/* //	atnotify(handle_display_picture_timer, 1); */
		/* //	alarm(sample_duration); */
		/* //	yield(); */
		/* //} */

		/* LOG("AV dist: %f, thresh: %f", */
			/* renderer_ctx->current_audio_pts - renderer_ctx->current_video_pts, 0.5 * sample_duration); */
		/* //if (renderer_ctx->video_ctx && */
		/* //	(renderer_ctx->current_audio_pts >= renderer_ctx->current_video_pts)) */
		/* if (renderer_ctx->video_ctx && */
			/* fabs(renderer_ctx->current_audio_pts - renderer_ctx->current_video_pts) <= 0.5 * sample_duration) */
		/* { */
			/* receive_picture(renderer_ctx, &videoPicture); */
			/* display_picture(renderer_ctx, &videoPicture); */
			/* //renderer_ctx->current_video_pts = videoPicture.pts; */
			/* //display_picture(renderer_ctx, &videoPicture); */
			/* if (videoPicture.frame) { */
				/* av_frame_unref(videoPicture.frame); */
				/* av_frame_free(&videoPicture.frame); */
			/* } */
		/* } */

		/* if (time_diff > 0) { */
			/* yield(); */
			/* LOG("sleeping %fms", time_diff); */
			/* sleep(time_diff); */
		/* } */

		/* free(audioSample.sample); */
	/* } */
/* } */
