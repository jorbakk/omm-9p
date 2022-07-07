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
// 1. AV sync
// - improve video smootheness (orange.ts)
// - remove audio delay (caused by samples in sdl queue?!)
// - fix 5.1 audio tracks playing faster
// 2. Keyboard events
// 3. 9P control server

// Thread layout:
//   main_thread (event loop)
//   -> decoder_thread
//      -> video_thread reads from video channel
//      -> audio_thread reads from audio channel


#include <u.h>
#include <time.h>  // posix std headers should be included between u.h and libc.h
#include <libc.h>
#include <9pclient.h>
#include <thread.h>
/* #include <keyboard.h> */

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


typedef struct RendererCtx
{
	AVFormatContext  *pFormatCtx;

	// Audio Stream.
	int                audioStream;
	AVStream          *audio_st;
	AVCodecContext    *audio_ctx;
	Channel           *audioq;
	uint8_t            audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) /2];
	unsigned int       audio_buf_size;
	unsigned int       audio_buf_index;
	AVFrame            audio_frame;
	AVPacket           audio_pkt;
	uint8_t *          audio_pkt_data;
	int                audio_pkt_size;
	double             audio_clock;
	int                audio_idx;
	double             audio_pts;
	double             current_audio_pts;
	int64_t            audio_start_rt;
	// Video Stream.
	int                videoStream;
	AVStream          *video_st;
	AVCodecContext    *video_ctx;
	SDL_Texture       *texture;
	SDL_Renderer      *renderer;
	Channel           *videoq;
	Channel           *pictq;
	struct SwsContext *sws_ctx;
	struct SwsContext *rgb_ctx;
	double             frame_timer;
	double             frame_last_pts;
	double             frame_last_delay;
	double             video_clock;
	double             video_current_pts;
	int64_t            video_current_pts_time;
	double             audio_diff_cum;
	double             audio_diff_avg_coef;
	double             audio_diff_threshold;
	int                audio_diff_avg_count;

	SDL_AudioSpec      specs;
	int                video_idx;
	double             video_pts;
	// AV Sync
	int	               av_sync_type;
	double             external_clock;
	int64_t            external_clock_time;
	// Seeking
	int	               seek_req;
	int	               seek_flags;
	int64_t            seek_pos;
	// Threads
	int                decode_tid;
	int                video_tid;
	int                audio_tid;
	// Input file name and plan 9 file reference
	char               filename[1024];
	CFid              *fid;
	// Keyboard
	/* Keyboardctl       *kbd; */
	// Quit flag
	int                quit;
	// Maximum number of frames to be decoded
	long               maxFramesToDecode;
	int	               currentFrameIndex;
	int                frame_fmt;
	SDL_AudioDeviceID  audioDevId;
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
    int         idx;
} VideoPicture;

typedef struct AudioSample
{
	/* uint8_t			 sample[(MAX_AUDIO_FRAME_SIZE * 3) /2]; */
	uint8_t	*sample;
	int size;
	int idx;
	double pts;
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
AVPacket flush_pkt;
char *addr = "tcp!localhost!5640";
char *aname;

void printHelp();
void saveFrame(AVFrame *pFrame, int width, int height, int frameIndex);
void video_display(RendererCtx *renderer_ctx, VideoPicture *videoPicture);
void savePicture(RendererCtx *renderer_ctx, VideoPicture *pPic, int frameIndex);
void decoder_thread(void *arg);
int stream_component_open(RendererCtx * renderer_ctx, int stream_index);
void video_thread(void *arg);
void audio_thread(void *arg);

static int audio_resampling(
		RendererCtx *renderer_ctx,
		AVFrame * decoded_audio_frame,
		enum AVSampleFormat out_sample_fmt,
		uint8_t * out_buf
);
AudioResamplingState * getAudioResampling(uint64_t channel_layout);
void stream_seek(RendererCtx *renderer_ctx, int64_t pos, int rel);
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
			p = name + strlen(name);
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
	RendererCtx *renderer_ctx = av_mallocz(sizeof(RendererCtx));
	// copy the file name input by the user to the RendererCtx structure
	av_strlcpy(renderer_ctx->filename, argv[1], sizeof(renderer_ctx->filename));
	// parse max frames to decode input by the user
	char * pEnd;
	renderer_ctx->maxFramesToDecode = strtol(argv[2], &pEnd, 10);
	renderer_ctx->av_sync_type = DEFAULT_AV_SYNC_TYPE;
	/* renderer_ctx->frame_fmt = FRAME_FMT_RGB; */
	renderer_ctx->frame_fmt = FRAME_FMT_YUV;
	renderer_ctx->video_ctx = NULL;
	renderer_ctx->audio_ctx = NULL;
	renderer_ctx->audio_idx = 0;
	renderer_ctx->audio_pts = 0;
	renderer_ctx->video_idx = 0;
	renderer_ctx->video_pts = 0;
	renderer_ctx->audio_only = 0;
	/* renderer_ctx->kbd = initkeyboard(""); */
	// Set up 9P connection
	LOG("opening 9P connection ...");
	CFid *fid = xopen(renderer_ctx->filename, OREAD);
	renderer_ctx->fid = fid;
	// Create picture and audio sample queue
	/* renderer_ctx->pictq = chancreate(sizeof(VideoPicture), VIDEO_PICTURE_QUEUE_SIZE); */
	/* renderer_ctx->audioq = chancreate(sizeof(AudioSample), MAX_AUDIOQ_SIZE); */
	audio_out = fopen("/tmp/out.pcm", "wb");
	// start the decoding thread to read data from the AVFormatContext
	renderer_ctx->decode_tid = threadcreate(decoder_thread, renderer_ctx, THREAD_STACK_SIZE);
	if (!renderer_ctx->decode_tid) {
		printf("Could not start decoder thread: %s.\n", SDL_GetError());
		av_free(renderer_ctx);
		return;
	}
	LOG("decoder thread created with id: %i", renderer_ctx->decode_tid);
	av_init_packet(&flush_pkt);
	flush_pkt.data = (uint8_t*)"FLUSH";
	for (;;) {
		yield();
		if (renderer_ctx->quit) {
			break;
		}
	}
	// FIXME never reached ... need to shut down the renderer properly
	LOG("freeing video state");
	av_free(renderer_ctx);
	/* closekeyboard(renderer_ctx->kbd); */
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
	RendererCtx * renderer_ctx = (RendererCtx *)arg;
	LOG("setting up IO context ...");
	unsigned char *avctxBuffer;
	avctxBuffer = malloc(avctxBufferSize);
	AVIOContext *pIOCtx = avio_alloc_context(
		avctxBuffer,		 // buffer
		avctxBufferSize,	 // buffer size
		0,				     // buffer is only readable - set to 1 for read/write
		renderer_ctx->fid,	 // user specified data
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
		sysfatal("Could not open file %s", renderer_ctx->filename);
	}
	LOG("opened stream input");
	// reset stream indexes
	renderer_ctx->videoStream = -1;
	renderer_ctx->audioStream = -1;
	// set global RendererCtx reference
	/* global_video_state = renderer_ctx; */
	renderer_ctx->pFormatCtx = pFormatCtx;
	ret = avformat_find_stream_info(pFormatCtx, NULL);
	if (ret < 0) {
		LOG("Could not find stream information: %s.", renderer_ctx->filename);
		return;
	}
	if (_DEBUG_)
		av_dump_format(pFormatCtx, 0, renderer_ctx->filename, 0);
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
		ret = stream_component_open(renderer_ctx, videoStream);
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
		ret = stream_component_open(renderer_ctx, audioStream);
		// check audio codec was opened correctly
		if (ret < 0) {
			LOG("Could not open audio codec.");
			goto fail;
		}
		LOG("audio stream component opened successfully.");
	}
	if (renderer_ctx->videoStream < 0 && renderer_ctx->audioStream < 0) {
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

	// Allocate frames and buffers for converting/scaling decoded frames to RGB and YUV
	// and creating a copy to be send through the video picture channel
    static AVFrame *pFrameRGB = NULL;
    uint8_t * buffer = NULL;
    uint8_t * yuvbuffer = NULL;
	if (renderer_ctx->video_ctx) {
	    pFrameRGB = av_frame_alloc();
	    int numBytes;
	    numBytes = av_image_get_buffer_size(
			AV_PIX_FMT_RGB24,
			renderer_ctx->video_ctx->width,
			renderer_ctx->video_ctx->height,
			32
			);
	    buffer = (uint8_t *) av_malloc(numBytes * sizeof(uint8_t));
	    av_image_fill_arrays(
			pFrameRGB->data,
			pFrameRGB->linesize,
			buffer,
			AV_PIX_FMT_RGB24,
			renderer_ctx->video_ctx->width,
			renderer_ctx->video_ctx->height,
			32
	    );

	    int yuvNumBytes = av_image_get_buffer_size(
			AV_PIX_FMT_YUV420P,
			renderer_ctx->video_ctx->width,
			renderer_ctx->video_ctx->height,
			32
			);
		yuvbuffer = (uint8_t *) av_malloc(yuvNumBytes * sizeof(uint8_t));
	}

	// Main decoder loop
	for (;;) {
		if (renderer_ctx->quit) {
			break;
		}
		int demuxer_ret = av_read_frame(renderer_ctx->pFormatCtx, packet);
		LOG("read av packet of size: %i", packet->size);
		if (demuxer_ret < 0) {
			LOG("failed to read av packet: %s", av_err2str(demuxer_ret));
			if (demuxer_ret == AVERROR_EOF) {
				LOG("EOF");
				// media EOF reached, quit
				renderer_ctx->quit = 1;
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
		if (packet->stream_index == renderer_ctx->videoStream) {
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
		}
		if (decsend_ret < 0) {
			LOG("error sending packet to decoder: %s", av_err2str(decsend_ret));
			return;
		}
		// This loop is only needed when we get more than one decoded frame out
		// of one packet read from the demuxer
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
			if (codecCtx == renderer_ctx->video_ctx) {
				VideoPicture videoPicture = {
					.frame = NULL,
					.rgbbuf = NULL,
					.planes = NULL,
					.width = codecCtx->width,
					.height = codecCtx->height
					/* .pts = codecCtx->frame_number */
					};
				if (renderer_ctx->frame_fmt == FRAME_FMT_PRISTINE) {
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
				else if (renderer_ctx->frame_fmt == FRAME_FMT_RGB) {
		            sws_scale(
		                renderer_ctx->rgb_ctx,
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
				else if (renderer_ctx->frame_fmt == FRAME_FMT_YUV) {
				    videoPicture.frame = av_frame_alloc();
					av_image_fill_arrays(
							videoPicture.frame->data,
							videoPicture.frame->linesize,
							yuvbuffer,
							AV_PIX_FMT_YUV420P,
							codecCtx->width,
							codecCtx->height,
							32
					);
		            sws_scale(
		                renderer_ctx->sws_ctx,
		                (uint8_t const * const *)pFrame->data,
		                pFrame->linesize,
		                0,
		                codecCtx->height,
		                videoPicture.frame->data,
		                videoPicture.frame->linesize
		            );
			    }
				renderer_ctx->video_idx++;
				videoPicture.idx = renderer_ctx->video_idx;
				/* double frame_duration = 1000.0 / av_q2d(codecCtx->framerate); */
				double frame_duration = 1000.0 * av_q2d(codecCtx->time_base);
				LOG("video frame duration: %fms, fps: %f", frame_duration, 1000.0 / frame_duration);
				renderer_ctx->video_pts += frame_duration;
				videoPicture.pts = renderer_ctx->video_pts;
				if (!renderer_ctx->audio_only) {
					LOG("==> sending picture with idx: %d, pts: %f to picture queue ...", videoPicture.idx, videoPicture.pts);
					int sendret = send(renderer_ctx->pictq, &videoPicture);
					if (sendret == 1) {
						LOG("==> sending picture with idx: %d, pts: %f to picture queue succeeded.", videoPicture.idx, videoPicture.pts);
					}
					else if (sendret == -1) {
						LOG("==> sending picture to picture queue interrupted");
					}
					else {
						LOG("==> unforseen error when sending picture to picture queue");
					}
				}
			}
			else if (codecCtx == renderer_ctx->audio_ctx) {
				if (renderer_ctx->audio_idx > renderer_ctx->maxFramesToDecode) {
					LOG("max frames reached");
					threadexitsall("max frames reached");
				}
				int data_size = audio_resampling(
						renderer_ctx,
						pFrame,
						AV_SAMPLE_FMT_S16,
						renderer_ctx->audio_buf);
				av_frame_unref(pFrame);
				LOG("resampled audio bytes: %d", data_size);
				AudioSample audioSample = {
					/* .sample = renderer_ctx->audio_buf, */
					.size = data_size
					};
				renderer_ctx->audio_idx++;
				audioSample.idx = renderer_ctx->audio_idx;
				int bytes_per_sec = 2 * codecCtx->sample_rate * codecCtx->channels;
				double sample_duration = 1000.0 * audioSample.size / bytes_per_sec;
				/* double sample_duration = 0.5 * 1000.0 * audioSample.size / bytes_per_sec; */
				/* double sample_duration = 2 * 1000.0 * audioSample.size / bytes_per_sec; */
				LOG("audio sample duration: %fms", sample_duration);
				renderer_ctx->audio_pts += sample_duration;  // audio sample length in ms
				audioSample.pts = renderer_ctx->audio_pts;
				audioSample.sample = malloc(sizeof(renderer_ctx->audio_buf));
				memcpy(audioSample.sample, renderer_ctx->audio_buf, sizeof(renderer_ctx->audio_buf));
				int sendret = send(renderer_ctx->audioq, &audioSample);
				if (sendret == 1) {
					LOG("==> sending audio sample with idx: %d, pts: %f to audio queue succeeded.", audioSample.idx, audioSample.pts);
					/* LOG("==> sending audio sample to audio queue succeeded."); */
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
	if (renderer_ctx->videoq) {
		chanfree(renderer_ctx->videoq);
	}
	if (renderer_ctx->audioq) {
		chanfree(renderer_ctx->audioq);
	}
	if (renderer_ctx->pictq) {
		chanfree(renderer_ctx->pictq);
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


int
stream_component_open(RendererCtx * renderer_ctx, int stream_index)
{
	LOG("opening stream component");
	AVFormatContext * pFormatCtx = renderer_ctx->pFormatCtx;
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
		LOG("setting up audio device with requested specs - sample_rate: %d, channels: %d ...", codecCtx->sample_rate, codecCtx->channels);
		SDL_AudioSpec wanted_specs;
		/* SDL_AudioSpec specs; */
		/* codecCtx->channels = 2; */
		wanted_specs.freq = codecCtx->sample_rate;
		wanted_specs.format = AUDIO_S16SYS;
		wanted_specs.channels = codecCtx->channels;
		wanted_specs.silence = 0;
		wanted_specs.samples = SDL_AUDIO_BUFFER_SIZE;
		/* // SDL threading when entering the audio_callback crashes ... */
		/* wanted_specs.callback = audio_callback; */
		wanted_specs.callback = NULL;
		wanted_specs.userdata = renderer_ctx;
		/* ret = SDL_OpenAudio(&wanted_specs, &specs); */
		/* if (ret < 0) { */
			/* printf("SDL_OpenAudio: %s.\n", SDL_GetError()); */
			/* return -1; */
		/* } */
		/* renderer_ctx->audioDevId = SDL_OpenAudioDevice(NULL, 0, &wanted_specs, &specs, 0); */
		renderer_ctx->audioDevId = SDL_OpenAudioDevice(NULL, 0, &wanted_specs, &renderer_ctx->specs, 0);
		if (renderer_ctx->audioDevId == 0) {
			printf("SDL_OpenAudio: %s.\n", SDL_GetError());
			return -1;
		}
		LOG("audio device with id: %d opened successfully", renderer_ctx->audioDevId);
		LOG("audio specs are freq: %d, channels: %d", renderer_ctx->specs.freq, renderer_ctx->specs.channels);
	}
	if (avcodec_open2(codecCtx, codec, NULL) < 0) {
		printf("Unsupported codec.\n");
		return -1;
	}
	switch (codecCtx->codec_type) {
		case AVMEDIA_TYPE_AUDIO:
		{
			renderer_ctx->audioStream = stream_index;
			renderer_ctx->audio_st = pFormatCtx->streams[stream_index];
			renderer_ctx->audio_ctx = codecCtx;
			renderer_ctx->audio_buf_size = 0;
			renderer_ctx->audio_buf_index = 0;
			memset(&renderer_ctx->audio_pkt, 0, sizeof(renderer_ctx->audio_pkt));
			/* renderer_ctx->audioq = chancreate(sizeof(AVPacket*), MAX_AUDIOQ_SIZE); */
			renderer_ctx->audioq = chancreate(sizeof(AVPacket), MAX_AUDIOQ_SIZE);
			renderer_ctx->audio_tid = threadcreate(audio_thread, renderer_ctx, THREAD_STACK_SIZE);
			LOG("Audio thread created with id: %i", renderer_ctx->audio_tid);
			LOG("calling sdl_pauseaudio(0) ...");
			/* SDL_PauseAudio(0); */
			SDL_PauseAudioDevice(renderer_ctx->audioDevId, 0);
			LOG("sdl_pauseaudio(0) called.");
		}
			break;
		case AVMEDIA_TYPE_VIDEO:
		{
			renderer_ctx->videoStream = stream_index;
			renderer_ctx->video_st = pFormatCtx->streams[stream_index];
			renderer_ctx->video_ctx = codecCtx;
			// Initialize the frame timer and the initial
			// previous frame delay: 1ms = 1e-6s
			renderer_ctx->frame_timer = (double)av_gettime() / 1000000.0;
			renderer_ctx->frame_last_delay = 40e-3;
			renderer_ctx->video_current_pts_time = av_gettime();
			/* renderer_ctx->videoq = chancreate(sizeof(AVPacket*), MAX_VIDEOQ_SIZE); */
			/* renderer_ctx->pictq = chancreate(sizeof(VideoPicture*), VIDEO_PICTURE_QUEUE_SIZE); */
			/* renderer_ctx->videoq = chancreate(sizeof(AVPacket), MAX_VIDEOQ_SIZE); */
			renderer_ctx->pictq = chancreate(sizeof(VideoPicture), VIDEO_PICTURE_QUEUE_SIZE);
			if (!renderer_ctx->audio_only) {
				renderer_ctx->video_tid = threadcreate(video_thread, renderer_ctx, THREAD_STACK_SIZE);
				LOG("Video thread created with id: %i", renderer_ctx->video_tid);
			}
			renderer_ctx->sws_ctx = sws_getContext(renderer_ctx->video_ctx->width,
												 renderer_ctx->video_ctx->height,
												 renderer_ctx->video_ctx->pix_fmt,
												 renderer_ctx->video_ctx->width,
												 renderer_ctx->video_ctx->height,
												 AV_PIX_FMT_YUV420P,
												 SWS_BILINEAR,
												 NULL,
												 NULL,
												 NULL
			);
			renderer_ctx->rgb_ctx = sws_getContext(renderer_ctx->video_ctx->width,
												 renderer_ctx->video_ctx->height,
												 renderer_ctx->video_ctx->pix_fmt,
												 renderer_ctx->video_ctx->width,
												 renderer_ctx->video_ctx->height,
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
receive_pic(RendererCtx *renderer_ctx, VideoPicture *videoPicture)
{
	LOG("receiving picture from picture queue ...");
	int recret = recv(renderer_ctx->pictq, videoPicture);
	if (recret == 1) {
		LOG("<== received picture with idx: %d, pts: %f, current audio pts: %f", videoPicture->idx, videoPicture->pts, renderer_ctx->current_audio_pts);
	}
	else if (recret == -1) {
		LOG("<== reveiving picture from picture queue interrupted");
	}
	else {
		LOG("<== unforseen error when receiving picture from picture queue");
	}
}


void
video_thread(void *arg)
{
	RendererCtx *renderer_ctx = arg;
	VideoPicture videoPicture;
	receive_pic(renderer_ctx, &videoPicture);
	for (;;) {
		if (renderer_ctx->frame_fmt == FRAME_FMT_RGB) {
			savePicture(renderer_ctx, &videoPicture, (int)videoPicture.pts);
			receive_pic(renderer_ctx, &videoPicture);
			if (videoPicture.rgbbuf) {
				av_free(videoPicture.rgbbuf);
			}
		}
		else if (renderer_ctx->frame_fmt == FRAME_FMT_YUV) {
			LOG("picture with idx: %d, pts: %f, current audio pts: %f", videoPicture.idx, videoPicture.pts, renderer_ctx->current_audio_pts);
			if (renderer_ctx->current_audio_pts >= videoPicture.pts) {
				LOG("displaying picture");
				video_display(renderer_ctx, &videoPicture);
				if (videoPicture.frame) {
					av_frame_unref(videoPicture.frame);
					av_frame_free(&videoPicture.frame);
				}
				receive_pic(renderer_ctx, &videoPicture);
			}
			else {
				LOG("yielding video_thread");
				yield();
			}
		}
	}
}


void
audio_thread(void *arg)
{
	RendererCtx *renderer_ctx = arg;
	renderer_ctx->audio_start_rt = av_gettime();
	for (;;) {
		LOG("receiving and playing sample from audio queue ...");
		AudioSample audioSample;
		int recret = recv(renderer_ctx->audioq, &audioSample);
		if (recret == 1) {
			LOG("<== received audio sample with idx: %d, pts: %f from audio queue.", audioSample.idx, audioSample.pts);
			/* fwrite(audioSample.sample, 1, audioSample.size, audio_out); */
			int ret = SDL_QueueAudio(renderer_ctx->audioDevId, audioSample.sample, audioSample.size);
			if (ret < 0) {
				LOG("failed to write audio sample: %s", SDL_GetError());
			}
			else {
				LOG("audio sample idx: %d, size: %d, audio clock: %f written", audioSample.idx, audioSample.size, renderer_ctx->current_audio_pts);
				int audioq_size = SDL_GetQueuedAudioSize(renderer_ctx->audioDevId);
				int bytes_per_sec = 2 * renderer_ctx->audio_ctx->sample_rate * renderer_ctx->audio_ctx->channels;
				double queue_duration = 1000.0 * audioq_size / bytes_per_sec;
				int samples_queued = audioq_size / audioSample.size;
				double sample_duration = 1000.0 * audioSample.size / bytes_per_sec;
				LOG("sdl audio queue size in bytes: %d, msec: %f, samples: %d", audioq_size, queue_duration, samples_queued);
				/* renderer_ctx->current_audio_pts = audioSample.pts - queue_duration; */
				renderer_ctx->current_audio_pts = audioSample.pts;
				if (samples_queued > 5) {
					LOG("sleeping");
					sleep(sample_duration);
				}
				LOG("audio clock: %f, real time: %f", renderer_ctx->current_audio_pts, (av_gettime() - renderer_ctx->audio_start_rt) / 1000.0);
			}
			free(audioSample.sample);
		}
		else if (recret == -1) {
			LOG("<== reveiving audio sample from audio queue interrupted");
		}
		else {
			LOG("<== unforseen error when receiving audio sample from audio queue");
		}
	}
}


void
video_display(RendererCtx *renderer_ctx, VideoPicture *videoPicture)
{
	if (!screen) {
		// create a window with the specified position, dimensions, and flags.
		screen = SDL_CreateWindow(
			"FFmpeg SDL Video Player",
			SDL_WINDOWPOS_UNDEFINED,
			SDL_WINDOWPOS_UNDEFINED,
			renderer_ctx->video_ctx->width,
			renderer_ctx->video_ctx->height,
			SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI
			);
		SDL_GL_SetSwapInterval(1);
	}
	if (!screen) {
		printf("SDL: could not create window - exiting.\n");
		return;
	}
	if (!renderer_ctx->renderer) {
		// create a 2D rendering context for the SDL_Window
		renderer_ctx->renderer = SDL_CreateRenderer(screen, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
	}
	if (!renderer_ctx->texture) {
		// create a texture for a rendering context
		renderer_ctx->texture = SDL_CreateTexture(
			renderer_ctx->renderer,
			SDL_PIXELFORMAT_YV12,
			SDL_TEXTUREACCESS_STREAMING,
			renderer_ctx->video_ctx->width,
			renderer_ctx->video_ctx->height
			);
	}
	double aspect_ratio;
	int w, h, x, y;
	if (videoPicture->frame) {
		if (renderer_ctx->video_ctx->sample_aspect_ratio.num == 0) {
			aspect_ratio = 0;
		}
		else {
			aspect_ratio = av_q2d(renderer_ctx->video_ctx->sample_aspect_ratio) * renderer_ctx->video_ctx->width / renderer_ctx->video_ctx->height;
		}
		if (aspect_ratio <= 0.0) {
			aspect_ratio = (float)renderer_ctx->video_ctx->width /
						   (float)renderer_ctx->video_ctx->height;
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
		// set blit area x and y coordinates, width and height
		SDL_Rect rect;
		rect.x = x;
		rect.y = y;
		rect.w = w;
		rect.h = h;
		// update the texture with the new pixel data
		SDL_UpdateYUVTexture(
				renderer_ctx->texture,
				&rect,
				videoPicture->frame->data[0],
				videoPicture->frame->linesize[0],
				videoPicture->frame->data[1],
				videoPicture->frame->linesize[1],
				videoPicture->frame->data[2],
				videoPicture->frame->linesize[2]
		);
		// clear the current rendering target with the drawing color
		SDL_RenderClear(renderer_ctx->renderer);
		// copy a portion of the texture to the current rendering target
		SDL_RenderCopy(renderer_ctx->renderer, renderer_ctx->texture, NULL, NULL);
		// update the screen with any rendering performed since the previous call
		SDL_RenderPresent(renderer_ctx->renderer);
	}
}


int 
audio_resampling(RendererCtx * renderer_ctx, AVFrame * decoded_audio_frame, enum AVSampleFormat out_sample_fmt, uint8_t * out_buf)
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
	audioResampling->resampled_data = NULL;
	audioResampling->resampled_data_size = 0;
	return audioResampling;
}


void
savePicture(RendererCtx* renderer_ctx, VideoPicture *videoPicture, int frameIndex)
{
    AVFrame *pFrameRGB = NULL;
    uint8_t *buffer = NULL;
	if (renderer_ctx->frame_fmt == FRAME_FMT_PRISTINE) {
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
	if (renderer_ctx->frame_fmt == FRAME_FMT_PRISTINE) {
	    for (y = 0; y < pFrameRGB->height; y++) {
	        fwrite(pFrameRGB->data[0] + y * pFrameRGB->linesize[0], 1, videoPicture->width * 3, pFile);
	    }
    }
    else if (renderer_ctx->frame_fmt == FRAME_FMT_RGB) {
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
