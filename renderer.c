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


#include <time.h>
#include <sys/syscall.h>

#include <u.h>
#include <libc.h>
#include <signal.h>
#include <bio.h>
#include <fcall.h>
#include <9pclient.h>
#include <auth.h>
#include <thread.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#define _DEBUG_ 1
#define LOG(...) printloginfo(); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");

static struct timespec curtime;

void printloginfo(void)
{
    long            ms; // Milliseconds
    time_t          s;  // Seconds
	pid_t tid;
	tid = syscall(SYS_gettid);
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

void printHelp();
void saveFrame(AVFrame *avFrame, int width, int height, int frameIndex);

CFsys *(*nsmnt)(char*, char*) = nsmount;
CFsys *(*fsmnt)(int, char*) = fsmount;
static const int avctxBufferSize = 8192 * 10;
char *addr = "tcp!localhost!5640";
char *aname;


int
demuxerPacketRead(void *fid, uint8_t *buf, int count)
{
	LOG("demuxer reading %d bytes from fid: %p into buf: %p", count, fid, buf);
	CFid *cfid = (CFid*)fid;
	return fsread(cfid, buf, count);
}


int64_t
demuxerPacketSeek(void *fid, int64_t offset, int whence)
{
	LOG("demuxer seeking fid: %p offset: %ld", fid, offset);
	CFid *cfid = (CFid*)fid;
	return fsseek(cfid, offset, whence);
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
	} else {
		*path = name;
		if ((fd = dial(addr, nil, nil, nil)) < 0)
			sysfatal("dial: %r");
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
	fid = fsopen(fs, name, mode);
	if (fid == nil)
		sysfatal("fsopen %s: %r", name);
	return fid;
}


void
printHelp()
{
	LOG("Usage: ./renderer <filename> <max-frames-to-decode>");
}


void
saveFrame(AVFrame *avFrame, int width, int height, int frameIndex)
{
	FILE * pFile;
	char szFilename[32];
	int  y;
	sprintf(szFilename, "/tmp/frame%d.ppm", frameIndex);
	pFile = fopen(szFilename, "wb");
	if (pFile == NULL)
	{
		return;
	}
	fprintf(pFile, "P6\n%d %d\n255\n", width, height);
	for (y = 0; y < height; y++)
	{
		fwrite(avFrame->data[0] + y * avFrame->linesize[0], 1, width * 3, pFile);
	}
	fclose(pFile);
}


void
threadmain(int argc, char **argv)
{
	if (_DEBUG_)
		chatty9pclient = 1;

	if ( !(argc > 2) )
	{
		printHelp();
		return;
	}
	int ret = -1;

    ret = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER);
    if (ret != 0)
    {
        sysfatal("could not initialize SDL: %s", SDL_GetError());
    }

	// Set up 9P connection
	CFid *fid = xopen(argv[1], OREAD);

	// Set up IO
	unsigned char *avctxBuffer;
	avctxBuffer = malloc(avctxBufferSize);
	AVIOContext *pIOCtx = avio_alloc_context(
		avctxBuffer,		 // buffer
		avctxBufferSize,	 // buffer size
		0,				     // buffer is only readable - set to 1 for read/write
		fid,				 // user specified data
		demuxerPacketRead,   // function for reading packets
		NULL,				 // function for writing packets
		demuxerPacketSeek	 // function for seeking to position in stream
		);
	if(!pIOCtx){
		sysfatal("failed to allocate memory for ffmpeg av io context");
	}

	AVFormatContext *pFormatCtx = avformat_alloc_context();
	if (!pFormatCtx) {
	  sysfatal("failed to allocate av format context");
	}
	pFormatCtx->pb = pIOCtx;
	LOG("opening avformat input ...");
	ret = avformat_open_input(&pFormatCtx, NULL, NULL, NULL);
	if (ret < 0) {
	  sysfatal("open av format input: %s", av_err2str(ret));
	}

	ret = avformat_find_stream_info(pFormatCtx, NULL);
	if (ret < 0)
	{
		sysfatal("find stream information for %s: %s", argv[1], av_err2str(ret));
	}

	av_dump_format(pFormatCtx, 0, argv[1], 0);

	// Walk through the codec context until we find a video stream.
	int i;
	AVCodecContext *pCodecCtxOrig = NULL;
	AVCodecContext *pCodecCtx = NULL;
	// Find the first video stream
	int videoStream = -1;
	for (i = 0; i < pFormatCtx->nb_streams; i++)
	{
		if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			videoStream = i;
			break;
		}
	}
	if (videoStream == -1)
	{
		sysfatal("could not find a video stream");
	}

	// Find and open the video decoder
	AVCodec *pCodec = NULL;
	pCodec = avcodec_find_decoder(pFormatCtx->streams[videoStream]->codecpar->codec_id);
	if (pCodec == NULL)
	{
		sysfatal("unsupported codec");
	}
	pCodecCtxOrig = avcodec_alloc_context3(pCodec);
	ret = avcodec_parameters_to_context(pCodecCtxOrig, pFormatCtx->streams[videoStream]->codecpar);
	pCodecCtx = avcodec_alloc_context3(pCodec);
	ret = avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[videoStream]->codecpar);
	if (ret != 0)
	{
		sysfatal("copy codec context: %s", av_err2str(ret));
	}
	ret = avcodec_open2(pCodecCtx, pCodec, NULL);
	if (ret < 0)
	{
		sysfatal("open codec.");
	}

	// Decode frames
	AVFrame *pFrame = NULL;
	pFrame = av_frame_alloc();
	if (pFrame == NULL)
	{
		sysfatal("failed to allocate input frame.");
	}
	AVFrame *pFrameRGB = NULL;
	pFrameRGB = av_frame_alloc();
	if (pFrameRGB == NULL)
	{
		sysfatal("failed to allocate output frame");
	}
	uint8_t *buffer = NULL;
	int numBytes;
    numBytes = av_image_get_buffer_size(
                AV_PIX_FMT_YUV420P,
                pCodecCtx->width,
                pCodecCtx->height,
                32
            );
    buffer = (uint8_t *) av_malloc(numBytes * sizeof(uint8_t));
    AVFrame * pict = av_frame_alloc();
    av_image_fill_arrays(
        pict->data,
        pict->linesize,
        buffer,
        AV_PIX_FMT_YUV420P,
        pCodecCtx->width,
        pCodecCtx->height,
        32
    );

	// Create an SDL window
    SDL_Window * screen = SDL_CreateWindow( // [2]
                            "SDL Video Player",
                            SDL_WINDOWPOS_UNDEFINED,
                            SDL_WINDOWPOS_UNDEFINED,
                            pCodecCtx->width/2,
                            pCodecCtx->height/2,
                            SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI
    );
    if (!screen)
    {
        sysfatal("could not set sdl video mode: %s", SDL_GetError());
    }
    SDL_GL_SetSwapInterval(1);
    SDL_Renderer *renderer = NULL;
    renderer = SDL_CreateRenderer(screen, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
    SDL_Texture *texture = NULL;
    texture = SDL_CreateTexture(
                renderer,
                SDL_PIXELFORMAT_YV12,
                SDL_TEXTUREACCESS_STREAMING,
                pCodecCtx->width,
                pCodecCtx->height
            );

	// Read from the stream and write to output images
	struct SwsContext *sws_ctx = NULL;
	AVPacket *pPacket = av_packet_alloc();
	if (pPacket == NULL)
	{
		sysfatal("failed to alloc av-packet");
	}
    sws_ctx = sws_getContext(
        pCodecCtx->width,
        pCodecCtx->height,
        pCodecCtx->pix_fmt,
        pCodecCtx->width,
        pCodecCtx->height,
        AV_PIX_FMT_YUV420P,
        SWS_BILINEAR,
        NULL,
        NULL,
        NULL
    );
	int maxFramesToDecode;
	sscanf (argv[2], "%d", &maxFramesToDecode);
	i = 0;
	while (1)
	{
		ret = av_read_frame(pFormatCtx, pPacket);
		if (ret < 0) {
			LOG("read av packet: %s", av_err2str(ret));
			break;
		}
		if (pPacket->stream_index == videoStream)
		{
			ret = avcodec_send_packet(pCodecCtx, pPacket);
			if (ret < 0)
			{
				sysfatal("send packet for decoding: %s", av_err2str(ret));
			}

			while (ret >= 0)
			{
				ret = avcodec_receive_frame(pCodecCtx, pFrame);
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
				{
					LOG("no more frames available or end of file");
					break;
				}
				else if (ret < 0)
				{
					sysfatal("error while decoding: %s", av_err2str(ret));
				}
                sws_scale(
                    sws_ctx,
                    (uint8_t const * const *)pFrame->data,
                    pFrame->linesize,
                    0,
                    pCodecCtx->height,
                    pict->data,
                    pict->linesize
                );
				if (++i <= maxFramesToDecode)
				{
                    // get clip fps
                    double fps = av_q2d(pFormatCtx->streams[videoStream]->r_frame_rate);
                    // get clip sleep time
                    double sleep_time = 1.0/(double)fps;
                    // Use SDL_Delay in milliseconds to allow for cpu scheduling
                    SDL_Delay((1000 * sleep_time) - 10);
                    // (0,0) is the upper-left corner in SDL.
                    SDL_Rect rect;
                    rect.x = 0;
                    rect.y = 0;
                    rect.w = pCodecCtx->width;
                    rect.h = pCodecCtx->height;
                    // Use this function to update a rectangle within a planar
                    // YV12 or IYUV texture with new pixel data.
                    SDL_UpdateYUVTexture(
                        texture,            // the texture to update
                        &rect,              // a pointer to the rectangle of pixels to update, or NULL to update the entire texture
                        pict->data[0],      // the raw pixel data for the Y plane
                        pict->linesize[0],  // the number of bytes between rows of pixel data for the Y plane
                        pict->data[1],      // the raw pixel data for the U plane
                        pict->linesize[1],  // the number of bytes between rows of pixel data for the U plane
                        pict->data[2],      // the raw pixel data for the V plane
                        pict->linesize[2]   // the number of bytes between rows of pixel data for the V plane
                    );
                    // clear the current rendering target with the drawing color
                    SDL_RenderClear(renderer);
                    // copy a portion of the texture to the current rendering target
                    SDL_RenderCopy(
                        renderer,   // the rendering context
                        texture,    // the source texture
                        NULL,       // the source SDL_Rect structure or NULL for the entire texture
                        NULL        // the destination SDL_Rect structure or NULL for the entire rendering
                                    // target; the texture will be stretched to fill the given rectangle
                    );
                    // update the screen with any rendering performed since the previous call
                    SDL_RenderPresent(renderer);
				}
				else
				{
					break;
				}
			}

			if (i > maxFramesToDecode)
			{
				// exit loop and terminate
				break;
			}
		}
		av_packet_unref(pPacket);
	}

	av_free(buffer);
	av_frame_free(&pFrameRGB);
	av_free(pFrameRGB);
	av_frame_free(&pFrame);
	av_free(pFrame);
	avcodec_close(pCodecCtx);
	avcodec_close(pCodecCtxOrig);
	avformat_close_input(&pFormatCtx);
	free(avctxBuffer);
    SDL_DestroyRenderer(renderer);
    SDL_Quit();
}
