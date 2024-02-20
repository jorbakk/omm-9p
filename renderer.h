#ifndef RENDERER_INCLUDED
#define RENDERER_INCLUDED

#include <u.h>
#include <time.h>  // posix std headers should be included between u.h and libc.h
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include <9pclient.h>

#ifdef RENDER_FFMPEG
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#endif

#include <SDL2/SDL.h>

#define NCMD 9
#define NSTATE 8
#define THREAD_CREATE proccreate
#define THREAD_STACK_SIZE 1024 * 1024 * 10
#define SDL_AUDIO_BUFFER_SIZE 1024
#define VIDEO_PICTURE_QUEUE_SIZE 1


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

// Transitions is the State matrix with dimensions [cmds x current state]
// Each entry is the *next* state when cmd is received in current state.
// Commands in states LOAD, UNLOAD, ENGAGE, DISENG are ignored because
// read_cmd() is not called
int transitions[NCMD][NSTATE-1] = // no entry for EXIT state needed
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
	// Threads
	int                server_tid;
	int                decoder_tid;
	int                presenter_tid;
	int                pause_presenter_thread;
	Channel           *presq;
	// Audio output
	SDL_AudioDeviceID  audio_devid;
	// Video output
	SDL_Window        *sdl_window;
	SDL_Texture       *sdl_texture;
	SDL_Renderer      *sdl_renderer;
#ifdef RENDER_FFMPEG
#define MAX_AUDIO_FRAME_SIZE 192000
#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)
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
	int                audio_only;
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
	int                w, h, aw, ah;
	SDL_Rect           blit_copy_rect;
	AVRational         video_timebase;
	double             video_tbd;
#endif
} RendererCtx;

void blank_window(RendererCtx *rctx);
int resize_video(RendererCtx *rctx);


/// FIXME move the following into renderer_ffmpeg.c
typedef struct VideoPicture
{
	AVFrame    *frame;
	int         linesize;
	int         width;
	int         height;
	int         pix_fmt;
	double      pts;
	int         idx;
	int         eos;
} VideoPicture;

typedef struct AudioSample
{
	uint8_t	   *sample;
	int         size;
	int         idx;
	double      pts;
	double      duration;
	int         eos;
} AudioSample;

void display_picture(RendererCtx *rctx, VideoPicture *videoPicture);
int setup_format_ctx(RendererCtx *rctx);
int open_input_stream(RendererCtx *rctx);
void decoder_thread(void *arg);
void presenter_thread(void *arg);

#endif   /// RENDERER_INCLUDED
