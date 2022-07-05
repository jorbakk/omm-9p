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
// 1. Crash when playing iron.mp4
//    - malloc(): unsorted double linked list corrupted
//    - no issues with transport streams (also bunny_xxx.mp4 seems to be ok)
//    - maybe connected to freeing avframes, or sending packets through the channel
//    - no crashes in valgrind
//    - other possible causes: threading related, function returning s.th. w/o return statement
//    - decoded images are shifted / skewed (at least running in valgrind)
//    - valgrind with no options works best, otherwise also frames seem to be dropped
//    - the size of the video packets sent to the decoder are very small in the beginning
//      of the stream
//    - it seems to be always the same packet where sending to the decoder fails
//      packet size sequence: 382, 21318, 114, 56, 53, 89, 56, 80, 53, 66, 53, 53, 66, 53, crash
//    - need to check if the demuxer video and audio packet queues contain all the same pointer
//      of the one AVPacket that is allocated before the demuxer loop
//      ... checked: only received packet in audio queue changes, all other pointers remain
//      the same (in orig. sources).
// 2. Crash on writing audio stream to the pcm device using the SDL callback
//    - writing decoded audio to file works
//    - use a different audio output method w/o callback (that might use pthreads)

// 1. Write video pictures to framebuffer
// 2. Write audio samples to sound device
// 3. AV sync
// 4. Proper shutdown of renderer
// 5. Keyboard events
// 6. 9P control server

// Tutorial thread layout:
//   main_thread
//   -> demuxer_thread
//      -> stream_component_open (video)
//         -> video_thread
//      -> stream_component_open (audio)
//         -> audio_thread
//   -> event loop
//   -> display pictures

// Simplified thread layout:
//   main_thread
//   -> decoder_thread
//      -> video_thread
//      -> audio_thread
//         -> clock_thread (display/render)
//   -> event loop

// Current thread layout:
//   main_thread (event loop)
//   -> decoder_thread
//   -> display_thread

#include <u.h>
#include <time.h>  // posix std headers should be included between u.h and libc.h
#include <libc.h>
#include <signal.h>
#include <bio.h>
#include <fcall.h>
#include <9pclient.h>
#include <auth.h>
#include <thread.h>

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
#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000
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

#define FRAME_FMT_PRISTINE 0
#define FRAME_FMT_RGB 1
#define FRAME_FMT_YUV 2


typedef struct VideoState
{
	AVFormatContext * pFormatCtx;
	// Audio Stream.
	int				 audioStream;
	AVStream *		  audio_st;
	AVCodecContext *	audio_ctx;
	Channel         *audioq;
	uint8_t			 audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) /2];
	unsigned int		audio_buf_size;
	unsigned int		audio_buf_index;
	AVFrame			 audio_frame;
	AVPacket			audio_pkt;
	uint8_t *		   audio_pkt_data;
	int				 audio_pkt_size;
	double			  audio_clock;
	// Video Stream.
	int				 videoStream;
	AVStream *		  video_st;
	AVCodecContext *	video_ctx;
	SDL_Texture *	   texture;
	SDL_Renderer *	  renderer;
	Channel		 *videoq;
	Channel		*pictq;
	struct SwsContext * sws_ctx;
	struct SwsContext * rgb_ctx;
	double			  frame_timer;
	double			  frame_last_pts;
	double			  frame_last_delay;
	double			  video_clock;
	double			  video_current_pts;
	int64_t			 video_current_pts_time;
	double			  audio_diff_cum;
	double			  audio_diff_avg_coef;
	double			  audio_diff_threshold;
	int				 audio_diff_avg_count;
	// AV Sync
	int	 av_sync_type;
	double  external_clock;
	int64_t external_clock_time;
	// Seeking
	int	 seek_req;
	int	 seek_flags;
	int64_t seek_pos;
	// Threads.
	int decode_tid;
	int video_tid;
	int audio_tid;
	// Input file name and plan 9 file reference
	char filename[1024];
	CFid *fid;
	// Quit flag
	int quit;
	// Maximum number of frames to be decoded.
	long	maxFramesToDecode;
	int	 currentFrameIndex;
	int frame_fmt;
} VideoState;

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

typedef struct VideoPicture
{
    AVFrame *   frame;
    uint8_t     *rgbbuf;
    int         linesize;
    /* uint8_t     *planes[AV_NUM_DATA_POINTERS]; */
    /* int         linesizes[AV_NUM_DATA_POINTERS]; */
    uint8_t     *planes[4];
    int         linesizes[4];
    int         width;
    int         height;
    int         pix_fmt;
    double      pts;
} VideoPicture;

typedef struct AudioSample
{
	/* uint8_t			 sample[(MAX_AUDIO_FRAME_SIZE * 3) /2]; */
	uint8_t	*sample;
	int size;
} AudioSample;

enum
{
	// Sync to audio clock.
	AV_SYNC_AUDIO_MASTER,
	// Sync to video clock.
	AV_SYNC_VIDEO_MASTER,
	// Sync to external clock: the computer clock
	AV_SYNC_EXTERNAL_MASTER,
};

SDL_Window *screen;
/* VideoState * global_video_state; */
AVPacket flush_pkt;
char *addr = "tcp!localhost!5640";
char *aname;

void printHelp();
void saveFrame(AVFrame *pFrame, int width, int height, int frameIndex);
void video_display(VideoState *videoState, VideoPicture *videoPicture);
void savePicture(VideoState* videoState, VideoPicture *pPic, int frameIndex);
void decoder_thread(void * arg);
int stream_component_open(
		VideoState * videoState,
		int stream_index
);
void video_thread(void *arg);
void audio_thread(void *arg);
/* static int64_t guess_correct_pts( */
		/* AVCodecContext * ctx, */
		/* int64_t reordered_pts, */
		/* int64_t dts */
/* ); */
double synchronize_video(
		VideoState * videoState,
		AVFrame * src_frame,
		double pts
);
int synchronize_audio(
		VideoState * videoState,
		short * samples,
		int samples_size
);
void video_refresh_timer(void * userdata);
double get_audio_clock(VideoState * videoState);
double get_video_clock(VideoState * videoState);
double get_external_clock(VideoState * videoState);
double get_master_clock(VideoState * videoState);
static void schedule_refresh(
		VideoState * videoState,
		Uint32 delay
);
/* void audio_callback( */
		/* void * userdata, */
		/* Uint8 * stream, */
		/* int len */
/* ); */
/* int audio_decode_frame( */
		/* VideoState * videoState, */
		/* uint8_t * audio_buf, */
		/* int buf_size, */
		/* double * pts_ptr */
/* ); */
static int audio_resampling(
		VideoState * videoState,
		AVFrame * decoded_audio_frame,
		enum AVSampleFormat out_sample_fmt,
		uint8_t * out_buf
);
AudioResamplingState * getAudioResampling(uint64_t channel_layout);
void stream_seek(VideoState * videoState, int64_t pos, int rel);
CFsys *(*nsmnt)(char*, char*) = nsmount;
CFsys *(*fsmnt)(int, char*) = fsmount;


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


CFsys*
xparse(char *name, char **path)
{
	int fd;
	char *p;
	CFsys *fs;

	if (addr == nil) {
		p = strchr(name, '/');
		if(p == nil)
			p = name+strlen(name);
		else
			*p++ = 0;
		*path = p;
		fs = nsmnt(name, aname);
		if(fs == nil)
			sysfatal("mount: %r");
	}
	else {
		LOG("setting path to: %s", name);
		*path = name;
		LOG("dialing address: %s ...", addr);
		if ((fd = dial(addr, nil, nil, nil)) < 0)
			sysfatal("dial: %r");
		LOG("mounting address ...");
		if ((fs = fsmnt(fd, aname)) == nil)
			sysfatal("mount: %r");
	}
	return fs;
}


CFid*
xopen(char *name, int mode)
{
	CFid *fid;
	CFsys *fs;

	fs = xparse(name, &name);
	LOG("opening: %s", name);
	fid = fsopen(fs, name, mode);
	if (fid == nil)
		sysfatal("fsopen %s: %r", name);
	return fid;
}

static FILE *audio_out;

void
threadmain(int argc, char **argv)
{
	if (_DEBUG_)
		chatty9pclient = 1;
	if (argc != 3) {
		printHelp();
		return;
	}
	int ret = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER);
	if (ret != 0) {
		printf("Could not initialize SDL - %s\n.", SDL_GetError());
		return;
	}
	VideoState * videoState = NULL;
	videoState = av_mallocz(sizeof(VideoState));
	// copy the file name input by the user to the VideoState structure
	av_strlcpy(videoState->filename, argv[1], sizeof(videoState->filename));
	// parse max frames to decode input by the user
	char * pEnd;
	videoState->maxFramesToDecode = strtol(argv[2], &pEnd, 10);
	/* schedule_refresh(videoState, 100); */
	videoState->av_sync_type = DEFAULT_AV_SYNC_TYPE;
	/* videoState->frame_fmt = FRAME_FMT_RGB; */
	videoState->frame_fmt = FRAME_FMT_YUV;
	// Set up 9P connection
	LOG("opening 9P connection ...");
	CFid *fid = xopen(videoState->filename, OREAD);
	videoState->fid = fid;
	// Create picture and audio sample queue
	/* videoState->pictq = chancreate(sizeof(VideoPicture), VIDEO_PICTURE_QUEUE_SIZE); */
	/* videoState->audioq = chancreate(sizeof(AudioSample), MAX_AUDIOQ_SIZE); */
	audio_out = fopen("/tmp/out.pcm", "wb");
	// start the decoding thread to read data from the AVFormatContext
	videoState->decode_tid = threadcreate(decoder_thread, videoState, THREAD_STACK_SIZE);
	if (!videoState->decode_tid) {
		printf("Could not start decoder thread: %s.\n", SDL_GetError());
		av_free(videoState);
		return;
	}
	LOG("decoder thread created with id: %i", videoState->decode_tid);
	av_init_packet(&flush_pkt);
	flush_pkt.data = (uint8_t*)"FLUSH";
	for (;;) {
		yield();
		/* video_thread(videoState); */
		// FIXME receiving audio and video frames from their queues is not balanced
		/* audio_thread(videoState); */
		/* audio_thread(videoState); */
		if (videoState->quit) {
			break;
		}
	}
	// FIXME never reached ... need to shut down the renderer properly
	LOG("freeing video state");
	av_free(videoState);
	return;
}


void printHelp()
{
	printf("Invalid arguments.\n\n");
	printf("Usage: renderer <filename> <max-frames-to-decode>\n\n");
}


void decoder_thread(void * arg)
{
	LOG("decoder thread started");
	VideoState * videoState = (VideoState *)arg;
	LOG("setting up IO context ...");
	unsigned char *avctxBuffer;
	avctxBuffer = malloc(avctxBufferSize);
	AVIOContext *pIOCtx = avio_alloc_context(
		avctxBuffer,		 // buffer
		avctxBufferSize,	 // buffer size
		0,				     // buffer is only readable - set to 1 for read/write
		videoState->fid,	 // user specified data
		demuxerPacketRead,   // function for reading packets
		NULL,				 // function for writing packets
		demuxerPacketSeek	 // function for seeking to position in stream
		);
	if(!pIOCtx) {
		sysfatal("failed to allocate memory for ffmpeg av io context");
	}
	AVFormatContext *pFormatCtx = avformat_alloc_context();
	if (!pFormatCtx) {
	  sysfatal("failed to allocate av format context");
	}
	pFormatCtx->pb = pIOCtx;
	LOG("opening stream input ...");
	int ret = avformat_open_input(&pFormatCtx, NULL, NULL, NULL);
	if (ret < 0) {
		sysfatal("Could not open file %s", videoState->filename);
	}
	LOG("opened stream input");
	// reset stream indexes
	videoState->videoStream = -1;
	videoState->audioStream = -1;
	// set global VideoState reference
	/* global_video_state = videoState; */
	videoState->pFormatCtx = pFormatCtx;
	ret = avformat_find_stream_info(pFormatCtx, NULL);
	if (ret < 0) {
		LOG("Could not find stream information: %s.", videoState->filename);
		return;
	}
	if (_DEBUG_)
		av_dump_format(pFormatCtx, 0, videoState->filename, 0);
	int videoStream = -1;
	int audioStream = -1;
	for (int i = 0; i < pFormatCtx->nb_streams; i++)
	{
		if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && videoStream < 0) {
			videoStream = i;
		}
		if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audioStream < 0) {
			audioStream = i;
		}
	}
	if (videoStream == -1) {
		LOG("Could not find video stream.");
		/* goto fail; */
	}
	else {
		ret = stream_component_open(videoState, videoStream);
		if (ret < 0) {
			printf("Could not open video codec.\n");
			goto fail;
		}
		LOG("video stream component opened successfully.");
	}
	if (audioStream == -1) {
		LOG("Could not find audio stream.");
		/* goto fail; */
	}
	else {
		ret = stream_component_open(videoState, audioStream);
		// check audio codec was opened correctly
		if (ret < 0) {
			LOG("Could not open audio codec.");
			goto fail;
		}
		LOG("audio stream component opened successfully.");
	}
	if (videoState->videoStream < 0 && videoState->audioStream < 0) {
		LOG("both video and audio stream missing");
		goto fail;
	}
	AVPacket *packet = av_packet_alloc();
	if (packet == NULL) {
		LOG("Could not allocate AVPacket.");
		goto fail;
	}
	static AVFrame *pFrame = NULL;
	pFrame = av_frame_alloc();
	if (!pFrame) {
		printf("Could not allocate AVFrame.\n");
		return;
	}

    static AVFrame *pFrameRGB = NULL;
    pFrameRGB = av_frame_alloc();
    uint8_t * buffer = NULL;
    int numBytes;
    numBytes = av_image_get_buffer_size(
		AV_PIX_FMT_RGB24,
		videoState->video_ctx->width,
		videoState->video_ctx->height,
		32
		);
    buffer = (uint8_t *) av_malloc(numBytes * sizeof(uint8_t));
    av_image_fill_arrays(
		pFrameRGB->data,
		pFrameRGB->linesize,
		buffer,
		AV_PIX_FMT_RGB24,
		videoState->video_ctx->width,
		videoState->video_ctx->height,
		32
    );

	// Main decoder loop
	for (;;) {
		if (videoState->quit) {
			break;
		}
		int demuxer_ret = av_read_frame(videoState->pFormatCtx, packet);
		LOG("read av packet of size: %i", packet->size);
		if (demuxer_ret < 0) {
			LOG("failed to read av packet: %s", av_err2str(demuxer_ret));
			if (demuxer_ret == AVERROR_EOF) {
				LOG("EOF");
				// media EOF reached, quit
				videoState->quit = 1;
				break;
			}
			else {
				// exit for loop in case of error
				break;
			}
		}
		if (packet->size == 0) {
			LOG("packet size is zero, exiting demuxer thread");
			break;
		}
		AVCodecContext *codecCtx = NULL;
		if (packet->stream_index == videoState->videoStream) {
			LOG("sending video packet of size %d to decoder", packet->size);
			codecCtx = videoState->video_ctx;
		}
		else {
			LOG("sending audio packet of size %d to decoder", packet->size);
			codecCtx = videoState->audio_ctx;
		}
		int decsend_ret = avcodec_send_packet(codecCtx, packet);
		LOG("sending packet of size %d to decoder returned %d: ", packet->size, decsend_ret);
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
		}
		if (decsend_ret < 0) {
			LOG("error sending packet to decoder: %s", av_err2str(decsend_ret));
			return;
		}
		// This loop is only needed when we get more than one decoded frame out
		// of on packet read from the demuxer
		int decoder_ret = 0;
		while (decoder_ret >= 0) {
			/* int frameFinished = 0; */
			// get decoded output data from decoder
			LOG("receiving decoded frame from decoder ...");
			decoder_ret = avcodec_receive_frame(codecCtx, pFrame);
			LOG("receiving returns: %d", decoder_ret);
			// check if entire frame was decoded
			if (decoder_ret == AVERROR(EAGAIN)) {
				LOG("av frame not ready: AVERROR = EAGAIN");
				break;
			}
			if (decoder_ret == AVERROR_EOF) {
				LOG("end of file: AVERROR = EOF");
				return;
			}
			if (decoder_ret == AVERROR(EINVAL)) {
				LOG("decoding error: AVERROR = EINVAL");
				return;
			}
			if (decoder_ret < 0) {
				LOG("error receiving decoded frame from decoder: %s", av_err2str(decoder_ret));
				return;
			}
			LOG("decoding frame finished");
			/* frameFinished = 1; */
			// TODO it would be nicer to check for the frame type instead for the codec context
			if (codecCtx == videoState->video_ctx) {
				if (codecCtx->frame_number > videoState->maxFramesToDecode) {
					LOG("max frames reached");
					threadexitsall("max frames reached");
				}
				VideoPicture videoPicture = {
					.frame = NULL,
					.rgbbuf = NULL,
					.planes = NULL,
					.width = codecCtx->width,
					.height = codecCtx->height,
					.pts = codecCtx->frame_number
					};
				if (videoState->frame_fmt == FRAME_FMT_PRISTINE) {
					// FIXME sending pristine frames over the video picture channel doesn't work
					videoPicture.pix_fmt = codecCtx->pix_fmt;
					LOG("AV_NUM_DATA_POINTERS: %d", AV_NUM_DATA_POINTERS);
					/* memcpy(videoPicture.linesizes, pFrame->linesize, 4 * sizeof(uint8_t*) / 8); */
					/* memcpy(videoPicture.linesizes, pFrame->linesize, AV_NUM_DATA_POINTERS); */
					/* memcpy(videoPicture.linesizes, pFrame->linesize, AV_NUM_DATA_POINTERS * sizeof(uint8_t*)); */
					/* memcpy(videoPicture.linesizes, pFrame->linesize, AV_NUM_DATA_POINTERS * sizeof(uint8_t*) / 8); */
					/* memcpy(videoPicture.planes, pFrame->data, AV_NUM_DATA_POINTERS * sizeof(uint8_t*)); */
					/* memcpy(videoPicture.planes, pFrame->data, 4 * sizeof(uint8_t*) / 8); */

					// FIXME avoid hardcoding parameter align to 32 ...
					LOG("allocating video picture for queueing ...");
					int frame_size = av_image_alloc(
						videoPicture.planes,
						pFrame->linesize,
						codecCtx->width,
						codecCtx->height,
						codecCtx->pix_fmt,
						32
					);
					USED(frame_size);
					LOG("copying video picture for queueing ...");
					av_image_copy(
						videoPicture.planes,
						//videoPicture.linesizes,
		                pFrame->linesize,
		                (uint8_t const**)pFrame->data,
		                pFrame->linesize,
						codecCtx->pix_fmt,
						codecCtx->width,
						codecCtx->height
					);

					/* av_frame_copy(); */
				}
				else if (videoState->frame_fmt == FRAME_FMT_RGB) {
		            sws_scale(
		                videoState->rgb_ctx,
		                (uint8_t const * const *)pFrame->data,
		                pFrame->linesize,
		                0,
		                codecCtx->height,
		                pFrameRGB->data,
		                pFrameRGB->linesize
		            );
					// av_frame_unref(pFrame);
					videoPicture.linesize = pFrameRGB->linesize[0];
				    int numBytes = av_image_get_buffer_size(
						AV_PIX_FMT_RGB24,
						codecCtx->width,
						codecCtx->height,
						32
						);
				    videoPicture.rgbbuf = (uint8_t *) av_malloc(numBytes * sizeof(uint8_t));
				    memcpy(videoPicture.rgbbuf, pFrameRGB->data[0], numBytes);
			    }
				else if (videoState->frame_fmt == FRAME_FMT_YUV) {
				    int numBytes = av_image_get_buffer_size(
						AV_PIX_FMT_YUV420P,
						codecCtx->width,
						codecCtx->height,
						32
						);
				    videoPicture.frame = av_frame_alloc();
					uint8_t * buffer = (uint8_t *) av_malloc(numBytes * sizeof(uint8_t));
					av_image_fill_arrays(
							videoPicture.frame->data,
							videoPicture.frame->linesize,
							buffer,
							AV_PIX_FMT_YUV420P,
							videoState->video_ctx->width,
							videoState->video_ctx->height,
							32
					);
		            sws_scale(
		                videoState->sws_ctx,
		                (uint8_t const * const *)pFrame->data,
		                pFrame->linesize,
		                0,
		                codecCtx->height,
		                videoPicture.frame->data,
		                videoPicture.frame->linesize
		            );
			    }
				LOG("==> sending picture with pts %f to picture queue ...", videoPicture.pts);
			    /* LOG( */
			        /* "Frame %c (%d) pts %ld dts %ld key_frame %d " */
			/* "[coded_picture_number %d, display_picture_number %d," */
			/* " %dx%d]", */
			        /* av_get_picture_type_char(pFrameRGB->pict_type), */
			        /* (int)videoPicture.pts, */
			        /* pFrameRGB->pts, */
			        /* pFrameRGB->pkt_dts, */
			        /* pFrameRGB->key_frame, */
			        /* pFrameRGB->coded_picture_number, */
			        /* pFrameRGB->display_picture_number, */
			        /* videoPicture.width, */
			        /* videoPicture.height */
			    /* ); */

				/* av_frame_unref(pFrameRGB); */
				int sendret = send(videoState->pictq, &videoPicture);
				if (sendret == 1) {
					LOG("==> sending picture with pts %f to picture queue succeeded.", videoPicture.pts);
				}
				else if (sendret == -1) {
					LOG("==> sending picture to picture queue interrupted");
				}
				else {
					LOG("==> unforseen error when sending picture to picture queue");
				}
			}
			else if (codecCtx == videoState->audio_ctx) {
				int data_size = audio_resampling(
						videoState,
						pFrame,
						AV_SAMPLE_FMT_S16,
						videoState->audio_buf);
				av_frame_unref(pFrame);
				LOG("resampled audio bytes: %d", data_size);
				AudioSample audioSample = {
					/* .sample = videoState->audio_buf, */
					.size = data_size
					};
				audioSample.sample = malloc(sizeof(videoState->audio_buf));
				memcpy(audioSample.sample, videoState->audio_buf, sizeof(videoState->audio_buf));
				int sendret = send(videoState->audioq, &audioSample);
				if (sendret == 1) {
					/* LOG("==> sending audio sample with pts %f to audio queue succeeded.", videoPicture.pts); */
					LOG("==> sending audio sample to audio queue succeeded.");
				}
				else if (sendret == -1) {
					LOG("==> sending audio sample to audio queue interrupted");
				}
				else {
					LOG("==> unforseen error when sending audio sample to audio queue");
				}
			}
			else {
				LOG("non AV packet from demuxer, ignoring");
			}
		}
		av_packet_unref(packet);
		av_frame_unref(pFrame);
	}
	// Clean up the decoder thread
	if (pIOCtx) {
		avio_context_free(&pIOCtx);
	}
	avformat_close_input(&pFormatCtx);
	if (pFormatCtx) {
		avformat_free_context(pFormatCtx);
	}
	if (videoState->videoq) {
		chanfree(videoState->videoq);
	}
	if (videoState->audioq) {
		chanfree(videoState->audioq);
	}
	if (videoState->pictq) {
		chanfree(videoState->pictq);
	}
	fclose(audio_out);
	// in case of failure, push the FF_QUIT_EVENT and return
	LOG("quitting demuxer thread");
	threadexitsall("end of file");
	fail:
	{
		// create a quit event
		return;
	};
}


int stream_component_open(VideoState * videoState, int stream_index)
{
	LOG("opening stream component");
	AVFormatContext * pFormatCtx = videoState->pFormatCtx;
	if (stream_index < 0 || stream_index >= pFormatCtx->nb_streams) {
		printf("Invalid stream index.");
		return -1;
	}
	AVCodec * codec = NULL;
	codec = avcodec_find_decoder(pFormatCtx->streams[stream_index]->codecpar->codec_id);
	if (codec == NULL) {
		printf("Unsupported codec.\n");
		return -1;
	}
	AVCodecContext * codecCtx = NULL;
	codecCtx = avcodec_alloc_context3(codec);
	int ret = avcodec_parameters_to_context(codecCtx, pFormatCtx->streams[stream_index]->codecpar);
	if (ret != 0) {
		printf("Could not copy codec context.\n");
		return -1;
	}
	if (codecCtx->codec_type == AVMEDIA_TYPE_AUDIO) {
		// FIXME disabling sdl audio for now ...
		/* LOG("setting up audio device ..."); */
		/* SDL_AudioSpec wanted_specs; */
		/* SDL_AudioSpec specs; */
		/* wanted_specs.freq = codecCtx->sample_rate; */
		/* wanted_specs.format = AUDIO_S16SYS; */
		/* wanted_specs.channels = codecCtx->channels; */
		/* wanted_specs.silence = 0; */
		/* wanted_specs.samples = SDL_AUDIO_BUFFER_SIZE; */
		/* // FIXME SDL threading when entering the audio_callback might crash ... */
		/* wanted_specs.callback = audio_callback; */
		/* wanted_specs.userdata = videoState; */
		/* ret = SDL_OpenAudio(&wanted_specs, &specs); */
		/* if (ret < 0) { */
			/* printf("SDL_OpenAudio: %s.\n", SDL_GetError()); */
			/* return -1; */
		/* } */
		/* LOG("audio device opened successfully"); */
	}
	if (avcodec_open2(codecCtx, codec, NULL) < 0) {
		printf("Unsupported codec.\n");
		return -1;
	}
	switch (codecCtx->codec_type) {
		case AVMEDIA_TYPE_AUDIO:
		{
			videoState->audioStream = stream_index;
			videoState->audio_st = pFormatCtx->streams[stream_index];
			videoState->audio_ctx = codecCtx;
			videoState->audio_buf_size = 0;
			videoState->audio_buf_index = 0;
			memset(&videoState->audio_pkt, 0, sizeof(videoState->audio_pkt));
			/* videoState->audioq = chancreate(sizeof(AVPacket*), MAX_AUDIOQ_SIZE); */
			videoState->audioq = chancreate(sizeof(AVPacket), MAX_AUDIOQ_SIZE);
			videoState->audio_tid = threadcreate(audio_thread, videoState, THREAD_STACK_SIZE);
			// FIXME disabling SDL audio for now ...
			/* LOG("calling sdl_pauseaudio(0) ..."); */
			/* SDL_PauseAudio(0); */
			/* LOG("sdl_pauseaudio(0) called."); */
		}
			break;
		case AVMEDIA_TYPE_VIDEO:
		{
			videoState->videoStream = stream_index;
			videoState->video_st = pFormatCtx->streams[stream_index];
			videoState->video_ctx = codecCtx;
			// Initialize the frame timer and the initial
			// previous frame delay: 1ms = 1e-6s
			videoState->frame_timer = (double)av_gettime() / 1000000.0;
			videoState->frame_last_delay = 40e-3;
			videoState->video_current_pts_time = av_gettime();
			/* videoState->videoq = chancreate(sizeof(AVPacket*), MAX_VIDEOQ_SIZE); */
			/* videoState->pictq = chancreate(sizeof(VideoPicture*), VIDEO_PICTURE_QUEUE_SIZE); */
			/* videoState->videoq = chancreate(sizeof(AVPacket), MAX_VIDEOQ_SIZE); */
			videoState->pictq = chancreate(sizeof(VideoPicture), VIDEO_PICTURE_QUEUE_SIZE);
			videoState->video_tid = threadcreate(video_thread, videoState, THREAD_STACK_SIZE);
			LOG("Video thread created with id: %i", videoState->video_tid);
			videoState->sws_ctx = sws_getContext(videoState->video_ctx->width,
												 videoState->video_ctx->height,
												 videoState->video_ctx->pix_fmt,
												 videoState->video_ctx->width,
												 videoState->video_ctx->height,
												 AV_PIX_FMT_YUV420P,
												 SWS_BILINEAR,
												 NULL,
												 NULL,
												 NULL
			);
			videoState->rgb_ctx = sws_getContext(videoState->video_ctx->width,
												 videoState->video_ctx->height,
												 videoState->video_ctx->pix_fmt,
												 videoState->video_ctx->width,
												 videoState->video_ctx->height,
												 AV_PIX_FMT_RGB24,
												 SWS_BILINEAR,
												 NULL,
												 NULL,
												 NULL
			);
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


void
video_thread(void *arg)
{
	VideoState *videoState = arg;
	for (;;) {
		LOG("receiving picture from picture queue and displaying video frame ...");
		VideoPicture videoPicture;
		int recret = recv(videoState->pictq, &videoPicture);
		if (recret == 1) {
			LOG("<== received picture with pts %f from picture queue.", videoPicture.pts);
		}
		else if (recret == -1) {
			LOG("<== reveiving picture from picture queue interrupted");
		}
		else {
			LOG("<== unforseen error when receiving picture from picture queue");
		}
		if (_DEBUG_) {
			LOG("Current Frame PTS:\t\t%f", videoPicture.pts);
			LOG("Last Frame PTS:\t\t%f", videoState->frame_last_pts);
		}

		/* AVFrame *pFrameRGB = videoPicture.frame; */
	    // save the read AVFrame into ppm file
		/* saveFrame(videoPicture.frame, videoPicture.width, videoPicture.height, (int)videoPicture.pts); */
		if (videoState->frame_fmt == FRAME_FMT_RGB) {
			savePicture(videoState, &videoPicture, (int)videoPicture.pts);
		}
		else if (videoState->frame_fmt == FRAME_FMT_YUV) {
			video_display(videoState, &videoPicture);
		}
		if (videoPicture.rgbbuf) {
			free(videoPicture.rgbbuf);
		}
		/* if (videoPicture.planes) { */
			/* free(videoPicture.planes); */
		/* } */

	    // print log information
	    /* LOG( */
	        /* "Frame %c (%d) pts %ld dts %ld key_frame %d " */
	/* "[coded_picture_number %d, display_picture_number %d," */
	/* " %dx%d]", */
	        /* av_get_picture_type_char(pFrameRGB->pict_type), */
	        /* (int)videoPicture.pts, */
	        /* pFrameRGB->pts, */
	        /* pFrameRGB->pkt_dts, */
	        /* pFrameRGB->key_frame, */
	        /* pFrameRGB->coded_picture_number, */
	        /* pFrameRGB->display_picture_number, */
	        /* videoPicture.width, */
	        /* videoPicture.height */
	    /* ); */
		/* // FIXME unreffing queued frames probably doesn't work */
		/* av_frame_unref(pFrameRGB); */
		LOG("receiving picture from picture queue and displaying video frame finished.");
	}
}


void
audio_thread(void *arg)
{
	VideoState *videoState = arg;
	for (;;) {
		LOG("receiving and playing sample from audio queue ...");
		AudioSample audioSample;
		int recret = recv(videoState->audioq, &audioSample);
		if (recret == 1) {
			/* LOG("<== received audio sample with pts %f from audio queue.", videoPicture.pts); */
			LOG("<== received audio sample from audio queue.");
		}
		else if (recret == -1) {
			LOG("<== reveiving audio sample from audio queue interrupted");
		}
		else {
			LOG("<== unforseen error when receiving audio sample from audio queue");
		}
		fwrite(audioSample.sample, 1, audioSample.size, audio_out);
		free(audioSample.sample);
	}
}











///////////////////////////////////////////////////////////////////////////////////////////////////



/* static int64_t guess_correct_pts(AVCodecContext * ctx, int64_t reordered_pts, int64_t dts) */
/* { */
	/* int64_t pts; */
	/* if (dts != AV_NOPTS_VALUE) { */
		/* ctx->pts_correction_num_faulty_dts += dts <= ctx->pts_correction_last_dts; */
		/* ctx->pts_correction_last_dts = dts; */
	/* } */
	/* else if (reordered_pts != AV_NOPTS_VALUE) { */
		/* ctx->pts_correction_last_dts = reordered_pts; */
	/* } */
	/* if (reordered_pts != AV_NOPTS_VALUE) { */
		/* ctx->pts_correction_num_faulty_pts += reordered_pts <= ctx->pts_correction_last_pts; */
		/* ctx->pts_correction_last_pts = reordered_pts; */
	/* } */
	/* else if (dts != AV_NOPTS_VALUE) { */
		/* ctx->pts_correction_last_pts = dts; */
	/* } */
	/* if ((ctx->pts_correction_num_faulty_pts <= ctx->pts_correction_num_faulty_dts || dts == AV_NOPTS_VALUE) && reordered_pts != AV_NOPTS_VALUE) { */
		/* pts = reordered_pts; */
	/* } */
	/* else { */
		/* pts = dts; */
	/* } */
	/* return pts; */
/* } */


double synchronize_video(VideoState * videoState, AVFrame * src_frame, double pts)
{
	double frame_delay;
	if (pts != 0) {
		// if we have pts, set video clock to it
		videoState->video_clock = pts;
	}
	else {
		// if we aren't given a pts, set it to the clock
		pts = videoState->video_clock;
	}
	// update the video clock
	frame_delay = av_q2d(videoState->video_ctx->time_base);
	// if we are repeating a frame, adjust clock accordingly
	frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);
	// increase video clock to match the delay required for repeaing frames
	videoState->video_clock += frame_delay;
	return pts;
}


int synchronize_audio(VideoState * videoState, short * samples, int samples_size)
{
	int n;
	double ref_clock;
	n = 2 * videoState->audio_ctx->channels;
	if (videoState->av_sync_type != AV_SYNC_AUDIO_MASTER) {
		double diff, avg_diff;
		int wanted_size, min_size, max_size /*, nb_samples */;
		ref_clock = get_master_clock(videoState);
		diff = get_audio_clock(videoState) - ref_clock;
		if (diff < AV_NOSYNC_THRESHOLD) {
			// accumulate the diffs
			videoState->audio_diff_cum = diff + videoState->audio_diff_avg_coef * videoState->audio_diff_cum;
			if (videoState->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
				videoState->audio_diff_avg_count++;
			}
			else {
				avg_diff = videoState->audio_diff_cum * (1.0 - videoState->audio_diff_avg_coef);
				/**
				 * So we're doing pretty well; we know approximately how off the audio
				 * is from the video or whatever we're using for a clock. So let's now
				 * calculate how many samples we need to add or lop off by putting this
				 * code where the "Shrinking/expanding buffer code" section is:
				 */
				if (fabs(avg_diff) >= videoState->audio_diff_threshold) {
					wanted_size = samples_size + ((int)(diff * videoState->audio_ctx->sample_rate) * n);
					min_size = samples_size * ((100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100);
					max_size = samples_size * ((100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100);
					if(wanted_size < min_size) {
						wanted_size = min_size;
					}
					else if (wanted_size > max_size) {
						wanted_size = max_size;
					}
					/**
					 * Now we have to actually correct the audio. You may have noticed that our
					 * synchronize_audio function returns a sample size, which will then tell us
					 * how many bytes to send to the stream. So we just have to adjust the sample
					 * size to the wanted_size. This works for making the sample size smaller.
					 * But if we want to make it bigger, we can't just make the sample size larger
					 * because there's no more data in the buffer! So we have to add it. But what
					 * should we add? It would be foolish to try and extrapolate audio, so let's
					 * just use the audio we already have by padding out the buffer with the
					 * value of the last sample.
					 */
					if(wanted_size < samples_size) {
						/* remove samples */
						samples_size = wanted_size;
					}
					else if (wanted_size > samples_size) {
						uint8_t *samples_end, *q;
						int nb;
						/* add samples by copying final sample*/
						nb = (samples_size - wanted_size);
						samples_end = (uint8_t *)samples + samples_size - n;
						q = samples_end + n;
						while(nb > 0)
						{
							memcpy(q, samples_end, n);
							q += n;
							nb -= n;
						}
						samples_size = wanted_size;
					}
				}
			}
		}
		else {
			/* difference is TOO big; reset diff stuff */
			videoState->audio_diff_avg_count = 0;
			videoState->audio_diff_cum = 0;
		}
	}
	return samples_size;
}


void video_refresh_timer(void * userdata)
{
	LOG("refresh timer");
	VideoState * videoState = (VideoState *)userdata;
	VideoPicture videoPicture;
	memset(&videoPicture, 0, sizeof(VideoPicture));
	double pts_delay;
	double audio_ref_clock;
	double sync_threshold;
	double real_delay;
	double audio_video_delay;
	if (videoState->video_st) {
		// check the VideoPicture queue contains decoded frames
		/* if (videoState->pictq_size == 0) */
		/* { */
			/* fprintf(stderr, "\n!!!videoState->pictq_size == 0!!!\n"); */

			/* schedule_refresh(videoState, 1); */
		/* } */
		/* else */
		/* { */
			// get VideoPicture reference using the queue read index
			/* videoPicture = recvp(videoState->pictq); */
			int recret = recv(videoState->pictq, &videoPicture);
			if (recret == 1) {
				LOG("<== received decoded video frame with pts %f from picture queue.", videoPicture.pts);
			}
			else if (recret == -1) {
				LOG("<== reveiving decoded video frame from picture queue interrupted");
			}
			else {
				LOG("<== unforseen error when receiving decoded video frame from picture queue");
			}
			if (_DEBUG_) {
				LOG("Current Frame PTS:\t\t%f", videoPicture.pts);
				LOG("Last Frame PTS:\t\t%f", videoState->frame_last_pts);
			}
			// get last frame pts
			pts_delay = videoPicture.pts - videoState->frame_last_pts;
			if (_DEBUG_) {
				LOG("PTS Delay:\t\t\t%f", pts_delay);
			}
			// if the obtained delay is incorrect
			if (pts_delay <= 0 || pts_delay >= 1.0) {
				// use the previously calculated delay
				pts_delay = videoState->frame_last_delay;
			}
			if (_DEBUG_) {
				LOG("Corrected PTS Delay:\t\t%f", pts_delay);
			}
			// save delay information for the next time
			videoState->frame_last_delay = pts_delay;
			videoState->frame_last_pts = videoPicture.pts;
			// in case the external clock is not used
			if (videoState->av_sync_type != AV_SYNC_VIDEO_MASTER) {
				// update delay to stay in sync with the master clock: audio or video
				audio_ref_clock = get_master_clock(videoState);
				if (_DEBUG_) {
					LOG("Ref Clock:\t\t\t%f", audio_ref_clock);
				}
				// calculate audio video delay accordingly to the master clock
				audio_video_delay = videoPicture.pts - audio_ref_clock;
				if (_DEBUG_) {
					LOG("Audio Video Delay:\t\t%f", audio_video_delay);
				}
				// skip or repeat the frame taking into account the delay
				sync_threshold = (pts_delay > AV_SYNC_THRESHOLD) ? pts_delay : AV_SYNC_THRESHOLD;
				if (_DEBUG_) {
					LOG("Sync Threshold:\t\t%f", sync_threshold);
				}
				// check audio video delay absolute value is below sync threshold
				if (fabs(audio_video_delay) < AV_NOSYNC_THRESHOLD) {
					if(audio_video_delay <= -sync_threshold) {
						pts_delay = 0;
					}
					else if (audio_video_delay >= sync_threshold) {
						pts_delay = 2 * pts_delay;
					}
				}
			}
			if (_DEBUG_) {
				LOG("Corrected PTS delay:\t\t%f", pts_delay);
			}
			videoState->frame_timer += pts_delay;
			// compute the real delay
			real_delay = videoState->frame_timer - (av_gettime() / 1000000.0);
			if (_DEBUG_) {
				LOG("Real Delay:\t\t\t%f", real_delay);
			}
			if (real_delay < 0.010) {
				real_delay = 0.010;
			}
			if (_DEBUG_) {
				LOG("Corrected Real Delay:\t\t%f", real_delay);
			}
			schedule_refresh(videoState, (Uint32)(real_delay * 1000 + 0.5));
			if (_DEBUG_) {
				LOG("Next Scheduled Refresh:\t%f", (real_delay * 1000 + 0.5));
			}
			/* video_display(videoState, &videoPicture); */
		/* } */
	}
	else {
		schedule_refresh(videoState, 100);
	}
	/* LOG("refresh timer finished."); */
}


double get_audio_clock(VideoState * videoState)
{
	double pts = videoState->audio_clock;
	int hw_buf_size = videoState->audio_buf_size - videoState->audio_buf_index;
	int bytes_per_sec = 0;
	int n = 2 * videoState->audio_ctx->channels;
	if (videoState->audio_st) {
		bytes_per_sec = videoState->audio_ctx->sample_rate * n;
	}
	if (bytes_per_sec) {
		pts -= (double) hw_buf_size / bytes_per_sec;
	}
	return pts;
}


double get_video_clock(VideoState * videoState)
{
	double delta = (av_gettime() - videoState->video_current_pts_time) / 1000000.0;
	return videoState->video_current_pts + delta;
}


double get_external_clock(VideoState * videoState)
{
	videoState->external_clock_time = av_gettime();
	videoState->external_clock = videoState->external_clock_time / 1000000.0;
	return videoState->external_clock;
}


double get_master_clock(VideoState * videoState)
{
	if (videoState->av_sync_type == AV_SYNC_VIDEO_MASTER) {
		return get_video_clock(videoState);
	}
	else if (videoState->av_sync_type == AV_SYNC_AUDIO_MASTER) {
		return get_audio_clock(videoState);
	}
	else if (videoState->av_sync_type == AV_SYNC_EXTERNAL_MASTER) {
		return get_external_clock(videoState);
	}
	else {
		fprintf(stderr, "Error: Undefined A/V sync type.");
		return -1;
	}
}


static void schedule_refresh(VideoState * videoState, Uint32 delay)
{
	// schedule an SDL timer
	// FIXME need to replace SDL_AddTimer with something else ...
	/* int ret = SDL_AddTimer(delay, sdl_refresh_timer_cb, videoState); */

	// check the timer was correctly scheduled
	/* if (ret == 0) */
	/* { */
		/* printf("Could not schedule refresh callback: %s.\n.", SDL_GetError()); */
	/* } */
}


// sdl_refresh_timer_cb() should not be called, as SDL_AddTimer() is deactivated in schedule_refresh()
// FIXME find a replacement for sdl_refresh_timer_cb ...
/* static Uint32 sdl_refresh_timer_cb(Uint32 interval, void * param) */
/* { */
	/* // create an SDL_Event of type FF_REFRESH_EVENT */
	/* SDL_Event event; */
	/* event.type = FF_REFRESH_EVENT; */
	/* event.user.data1 = param; */

	/* // push the event to the events queue */
	/* SDL_PushEvent(&event); */

	/* // return 0 to cancel the timer */
	/* return 0; */
/* } */


void video_display(VideoState *videoState, VideoPicture *videoPicture)
{
	if (!screen) {
		// create a window with the specified position, dimensions, and flags.
		screen = SDL_CreateWindow(
			"FFmpeg SDL Video Player",
			SDL_WINDOWPOS_UNDEFINED,
			SDL_WINDOWPOS_UNDEFINED,
			videoState->video_ctx->width,
			videoState->video_ctx->height,
			SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI
			);
		SDL_GL_SetSwapInterval(1);
	}
	if (!screen) {
		printf("SDL: could not create window - exiting.\n");
		return;
	}
	if (!videoState->renderer) {
		// create a 2D rendering context for the SDL_Window
		videoState->renderer = SDL_CreateRenderer(screen, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
	}
	if (!videoState->texture) {
		// create a texture for a rendering context
		videoState->texture = SDL_CreateTexture(
			videoState->renderer,
			SDL_PIXELFORMAT_YV12,
			SDL_TEXTUREACCESS_STREAMING,
			videoState->video_ctx->width,
			videoState->video_ctx->height
			);
	}
	double aspect_ratio;
	int w, h, x, y;
	if (videoPicture->frame) {
		if (videoState->video_ctx->sample_aspect_ratio.num == 0) {
			aspect_ratio = 0;
		}
		else {
			aspect_ratio = av_q2d(videoState->video_ctx->sample_aspect_ratio) * videoState->video_ctx->width / videoState->video_ctx->height;
		}
		if (aspect_ratio <= 0.0) {
			aspect_ratio = (float)videoState->video_ctx->width /
						   (float)videoState->video_ctx->height;
		}
		// get the size of a window's client area
		int screen_width;
		int screen_height;
		SDL_GetWindowSize(screen, &screen_width, &screen_height);
		// global SDL_Surface height
		h = screen_height;
		// retrieve width using the calculated aspect ratio and the screen height
		w = ((int) rint(h * aspect_ratio)) & -3;
		// if the new width is bigger than the screen width
		if (w > screen_width) {
			// set the width to the screen width
			w = screen_width;
			// recalculate height using the calculated aspect ratio and the screen width
			h = ((int) rint(w / aspect_ratio)) & -3;
		}
		// TODO: Add full screen support
		x = (screen_width - w);
		y = (screen_height - h);
		/* if (_DEBUG_) { */
			/* // dump information about the frame being rendered */
			/* LOG( */
					/* "Frame %c (%d) pts %" PRId64 " dts %" PRId64 " key_frame %d [coded_picture_number %d, display_picture_number %d, %dx%d]", */
					/* av_get_picture_type_char(videoPicture->frame->pict_type), */
					/* videoState->video_ctx->frame_number, */
					/* videoPicture->frame->pts, */
					/* videoPicture->frame->pkt_dts, */
					/* videoPicture->frame->key_frame, */
					/* videoPicture->frame->coded_picture_number, */
					/* videoPicture->frame->display_picture_number, */
					/* videoPicture->frame->width, */
					/* videoPicture->frame->height */
			/* ); */
		/* } */
		// set blit area x and y coordinates, width and height
		SDL_Rect rect;
		rect.x = x;
		rect.y = y;
		rect.w = w;
		rect.h = h;
		USED(rect);
		// update the texture with the new pixel data
		SDL_UpdateYUVTexture(
				videoState->texture,
				&rect,
				videoPicture->frame->data[0],
				videoPicture->frame->linesize[0],
				videoPicture->frame->data[1],
				videoPicture->frame->linesize[1],
				videoPicture->frame->data[2],
				videoPicture->frame->linesize[2]
		);
		// clear the current rendering target with the drawing color
		SDL_RenderClear(videoState->renderer);
		// copy a portion of the texture to the current rendering target
		SDL_RenderCopy(videoState->renderer, videoState->texture, NULL, NULL);
		// update the screen with any rendering performed since the previous call
		SDL_RenderPresent(videoState->renderer);
	}
}


/* static void packet_queue_flush(PacketQueue * queue) */
/* { */
	/* AVPacketList *pkt, *pkt1; */

	/* SDL_LockMutex(queue->mutex); */

	/* for (pkt = queue->first_pkt; pkt != NULL; pkt = pkt1) */
	/* { */
		/* pkt1 = pkt->next; */
		/* // ... warning about av_free_packet is depricated */
		/* av_free_packet(&pkt->pkt); */
		/* av_freep(&pkt); */
	/* } */

	/* queue->last_pkt = NULL; */
	/* queue->first_pkt = NULL; */
	/* queue->nb_packets = 0; */
	/* queue->size = 0; */

	/* SDL_UnlockMutex(queue->mutex); */
/* } */


/* void audio_callback(void * userdata, Uint8 * stream, int len) */
/* { */
	/* LOG("audio_callback() for SDL audio output ..."); */
	/* // retrieve the VideoState */
	/* VideoState * videoState = (VideoState *)userdata; */
	/* double pts; */
	/* // while the length of the audio data buffer is > 0 */
	/* while (len > 0) */
	/* { */
		/* LOG("audio_callback() looping over audio data buffer"); */
		/* // check global quit flag */
		/* if (global_video_state->quit) { */
			/* return; */
		/* } */
		/* // check how much audio is left to writes */
		/* if (videoState->audio_buf_index >= videoState->audio_buf_size) { */
			/* // we have already sent all avaialble data; get more */
			/* int audio_size = audio_decode_frame( */
									/* videoState, */
									/* videoState->audio_buf, */
									/* sizeof(videoState->audio_buf), */
									/* &pts */
							/* ); */
			/* if (audio_size < 0) { */
				/* // output silence */
				/* videoState->audio_buf_size = 1024; */

				/* // clear memory */
				/* memset(videoState->audio_buf, 0, videoState->audio_buf_size); */

				/* printf("audio_decode_frame() failed.\n"); */
			/* } */
			/* else { */
				/* audio_size = synchronize_audio(videoState, (int16_t *)videoState->audio_buf, audio_size); */

				/* // cast to usigned just to get rid of annoying warning messages */
				/* videoState->audio_buf_size = (unsigned)audio_size; */
			/* } */
			/* videoState->audio_buf_index = 0; */
		/* } */
		/* int len1 = videoState->audio_buf_size - videoState->audio_buf_index; */
		/* if (len1 > len) { */
			/* len1 = len; */
		/* } */
		/* // copy data from audio buffer to the SDL stream */
		/* LOG("audio_callback() copy data to sdl audio buffer ..."); */
		/* memcpy(stream, (uint8_t *)videoState->audio_buf + videoState->audio_buf_index, len1); */
		/* LOG("audio_callback() copied data to sdl audio buffer."); */
		/* len -= len1; */
		/* stream += len1; */
		/* // update global VideoState audio buffer index */
		/* videoState->audio_buf_index += len1; */
	/* } */
/* } */


static int audio_resampling(VideoState * videoState, AVFrame * decoded_audio_frame, enum AVSampleFormat out_sample_fmt, uint8_t * out_buf)
{
	// get an instance of the AudioResamplingState struct
	AudioResamplingState * arState = getAudioResampling(videoState->audio_ctx->channel_layout);
	if (!arState->swr_ctx) {
		printf("swr_alloc error.\n");
		return -1;
	}
	// get input audio channels
	arState->in_channel_layout = (videoState->audio_ctx->channels ==
								  av_get_channel_layout_nb_channels(videoState->audio_ctx->channel_layout)) ?
								 videoState->audio_ctx->channel_layout :
								 av_get_default_channel_layout(videoState->audio_ctx->channels);
	// check input audio channels correctly retrieved
	if (arState->in_channel_layout <= 0) {
		printf("in_channel_layout error.\n");
		return -1;
	}
	// set output audio channels based on the input audio channels
	if (videoState->audio_ctx->channels == 1) {
		arState->out_channel_layout = AV_CH_LAYOUT_MONO;
	}
	else if (videoState->audio_ctx->channels == 2) {
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
			videoState->audio_ctx->sample_rate,
			0
	);
	// Set SwrContext parameters for resampling
	av_opt_set_sample_fmt(
			arState->swr_ctx,
			"in_sample_fmt",
			videoState->audio_ctx->sample_fmt,
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
			videoState->audio_ctx->sample_rate,
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
			videoState->audio_ctx->sample_rate,
			videoState->audio_ctx->sample_rate,
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
			swr_get_delay(arState->swr_ctx, videoState->audio_ctx->sample_rate) + arState->in_nb_samples,
			videoState->audio_ctx->sample_rate,
			videoState->audio_ctx->sample_rate,
			AV_ROUND_UP
	);
	// check output samples number was correctly rescaled
	if (arState->out_nb_samples <= 0) {
		printf("av_rescale_rnd error\n");
		return -1;
	}
	if (arState->out_nb_samples > arState->max_out_nb_samples) {
		// free memory block and set pointer to NULL
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
		// free memory block and set pointer to NULL
		av_freep(&arState->resampled_data[0]);
	}
	av_freep(&arState->resampled_data);
	arState->resampled_data = NULL;
	if (arState->swr_ctx) {
		// free the allocated SwrContext and set the pointer to NULL
		swr_free(&arState->swr_ctx);
	}
	return arState->resampled_data_size;
}


AudioResamplingState * getAudioResampling(uint64_t channel_layout)
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
	audioResampling->resampled_data = NULL;
	audioResampling->resampled_data_size = 0;
	return audioResampling;
}


void stream_seek(VideoState * videoState, int64_t pos, int rel)
{
	if (!videoState->seek_req) {
		videoState->seek_pos = pos;
		videoState->seek_flags = rel < 0 ? AVSEEK_FLAG_BACKWARD : 0;
		videoState->seek_req = 1;
	}
}


void saveFrame(AVFrame *pFrame, int width, int height, int frameIndex)
{
	LOG("saving video picture to file ...");
    FILE * pFile;
    char szFilename[32];
    int  y;
    // Open file
    sprintf(szFilename, "/tmp/%06d.ppm", frameIndex);
    pFile = fopen(szFilename, "wb");
    if (pFile == NULL) {
        return;
    }
    fprintf(pFile, "P6\n%d %d\n255\n", width, height);
    for (y = 0; y < height; y++)
    {
        fwrite(pFrame->data[0] + y * pFrame->linesize[0], 1, width * 3, pFile);
    }
    fclose(pFile);
	LOG("saved video picture.");
}


void savePicture(VideoState* videoState, VideoPicture *videoPicture, int frameIndex)
{
    AVFrame *pFrameRGB = NULL;
    uint8_t *buffer = NULL;
	if (videoState->frame_fmt == FRAME_FMT_PRISTINE) {
		// Convert the video picture to the target format for saving to disk
	    pFrameRGB = av_frame_alloc();
	    int numBytes;
	    const int align = 32;
	    numBytes = av_image_get_buffer_size(
			AV_PIX_FMT_RGB24,
			videoPicture->width,
			videoPicture->height,
			align
			);
	    buffer = (uint8_t *) av_malloc(numBytes * sizeof(uint8_t));
	    av_image_fill_arrays(
			pFrameRGB->data,
			pFrameRGB->linesize,
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
											NULL,
											NULL,
											NULL
											);
	    sws_scale(
	        rgb_ctx,
	        (uint8_t const * const *)videoPicture->planes,
	        videoPicture->linesizes,
	        0,
	        videoPicture->height,
	        pFrameRGB->data,
	        pFrameRGB->linesize
	    );
    }
	LOG("saving video picture to file ...");
    FILE * pFile;
    char szFilename[32];
    int  y;
    // Open file
    sprintf(szFilename, "/tmp/%06d.ppm", frameIndex);
    pFile = fopen(szFilename, "wb");
    if (pFile == NULL) {
        return;
    }
    fprintf(pFile, "P6\n%d %d\n255\n", videoPicture->width, videoPicture->height);
	if (videoState->frame_fmt == FRAME_FMT_PRISTINE) {
	    for (y = 0; y < pFrameRGB->height; y++) {
	        fwrite(pFrameRGB->data[0] + y * pFrameRGB->linesize[0], 1, videoPicture->width * 3, pFile);
	    }
    }
    else if (videoState->frame_fmt == FRAME_FMT_RGB) {
	    for (y = 0; y < videoPicture->height; y++) {
	        fwrite(videoPicture->rgbbuf + y * videoPicture->linesize, 1, videoPicture->width * 3, pFile);
	    }
    }
    fclose(pFile);
    /* av_free_frame(pFrameRGB); */
	if (buffer) {
	    av_free(buffer);
    }
	LOG("saved video picture.");
}


// ... seek stuff:
		/* if(videoState->seek_req) */
		/* { */
			/* int video_stream_index = -1; */
			/* int audio_stream_index = -1; */
			/* int64_t seek_target_video = videoState->seek_pos; */
			/* int64_t seek_target_audio = videoState->seek_pos; */
			/* if (videoState->videoStream >= 0) { */
				/* video_stream_index = videoState->videoStream; */
			/* } */
			/* if (videoState->audioStream >= 0) { */
				/* audio_stream_index = videoState->audioStream; */
			/* } */
			/* if(video_stream_index >= 0 && audio_stream_index >= 0) { */
				/* seek_target_video = av_rescale_q(seek_target_video, AV_TIME_BASE_Q, pFormatCtx->streams[video_stream_index]->time_base); */
				/* seek_target_audio = av_rescale_q(seek_target_audio, AV_TIME_BASE_Q, pFormatCtx->streams[audio_stream_index]->time_base); */
			/* } */
			/* ret = av_seek_frame(videoState->pFormatCtx, video_stream_index, seek_target_video, videoState->seek_flags); */
			/* ret &= av_seek_frame(videoState->pFormatCtx, audio_stream_index, seek_target_audio, videoState->seek_flags); */
			/* if (ret < 0) { */
				/* // ... warning about FormatCtx->filename is depricated ... should vanish when switching to 9P */
				/* //fprintf(stderr, "%s: error while seeking\n", videoState->pFormatCtx->filename); */
			/* } */
			/* else { */
				/* if (videoState->videoStream >= 0) { */
					/* // FIXME need some means to flush a channel? */
					/* //packet_queue_flush(&videoState->videoq); */
					/* //packet_queue_put(&videoState->videoq, &flush_pkt); */
				/* } */
				/* if (videoState->audioStream >= 0) { */
					/* // FIXME need some means to flush a channel? */
					/* //packet_queue_flush(&videoState->audioq); */
					/* //packet_queue_put(&videoState->audioq, &flush_pkt); */
				/* } */
			/* } */
			/* videoState->seek_req = 0; */
		/* } */
		// check audio and video packets queues size
		// FIXME audioq and videoq now are Channels with unknown size, need to track the size separately ...?
		/* if (videoState->audioq.size > MAX_AUDIOQ_SIZE || videoState->videoq.size > MAX_VIDEOQ_SIZE) */
		/* { */
			/* // wait for audio and video queues to decrease size */
			/* SDL_Delay(10); */

			/* continue; */
		/* } */
