#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <assert.h>

#include "SDL2/SDL.h"
#include "SDL2/SDL_mutex.h"

#include "vlc/vlc.h"

#define WIDTH 640
#define HEIGHT 480

#define VIDEOWIDTH WIDTH
#define VIDEOHEIGHT HEIGHT

struct context {
	SDL_Renderer *renderer;
	SDL_Window *window;
	SDL_Texture *texture;
	SDL_mutex *mutex;
};

// VLC prepares to render a video frame.
static void *
lock(void *data, void **p_pixels)
{

	struct context *c = (struct context *)data;

	int pitch;
	SDL_LockMutex(c->mutex);
	SDL_LockTexture(c->texture, NULL, p_pixels, &pitch);

	return NULL;                // Picture identifier, not needed here.
}

// VLC just rendered a video frame.
static void
unlock(void *data, void *id, void *const *p_pixels)
{
	(void)id; (void)p_pixels;
	struct context *c = (struct context *)data;
	SDL_UnlockTexture(c->texture);
	SDL_UnlockMutex(c->mutex);
}

// VLC wants to display a video frame.
static void
display(void *data, void *id)
{
	(void)id;
	struct context *c = (struct context *)data;

	SDL_Rect rect;
	SDL_GetWindowSize(c->window, &rect.w, &rect.h);
	rect.x = 0;
	rect.y = 0;

	SDL_SetRenderDrawColor(c->renderer, 0, 80, 0, 255);
	SDL_RenderClear(c->renderer);
	SDL_RenderCopy(c->renderer, c->texture, NULL, &rect);
	SDL_RenderPresent(c->renderer);
}

static void
quit(int c)
{
	SDL_Quit();
	exit(c);
}

int
main(int argc, char *argv[])
{

	libvlc_instance_t *libvlc;
	libvlc_media_t *m;
	libvlc_media_player_t *mp;
	char const *vlc_argv[] = {
		// "--no-audio",           // Don't play audio.
		"--no-xlib",            // Don't use Xlib.
		"-v",
		// "--access access9P",
		// "-I dummy",
		// Apply a video filter.
		//"--video-filter", "sepia",
		//"--sepia-intensity=200"
	};
	int vlc_argc = sizeof(vlc_argv) / sizeof(*vlc_argv);

	SDL_Event event;
	int done = 0, action = 0, pause = 0;

	struct context context;

	if (argc < 2) {
		printf("Usage: %s <filename>\n", argv[0]);
		return EXIT_FAILURE;
	}
	// Initialise libSDL.
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		printf("Could not initialize SDL: %s.\n", SDL_GetError());
		return EXIT_FAILURE;
	}
	// Create SDL graphics objects.
	context.window = SDL_CreateWindow("Fartplayer",
	                                  SDL_WINDOWPOS_UNDEFINED,
	                                  SDL_WINDOWPOS_UNDEFINED,
	                                  WIDTH, HEIGHT,
	                                  SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
	if (!context.window) {
		fprintf(stderr, "Couldn't create window: %s\n", SDL_GetError());
		quit(3);
	}

	context.renderer = SDL_CreateRenderer(context.window, -1, 0);
	if (!context.renderer) {
		fprintf(stderr, "Couldn't create renderer: %s\n", SDL_GetError());
		quit(4);
	}

	context.texture = SDL_CreateTexture(context.renderer,
	                                    SDL_PIXELFORMAT_BGR565,
	                                    SDL_TEXTUREACCESS_STREAMING, VIDEOWIDTH,
	                                    VIDEOHEIGHT);
	if (!context.texture) {
		fprintf(stderr, "Couldn't create texture: %s\n", SDL_GetError());
		quit(5);
	}

	context.mutex = SDL_CreateMutex();

	// If you don't have this variable set you must have plugins directory
	// with the executable or libvlc_new() will not work!
	printf("VLC_PLUGIN_PATH=%s\n", getenv("VLC_PLUGIN_PATH"));

	// Initialise libVLC.
	libvlc = libvlc_new(vlc_argc, vlc_argv);
	if (NULL == libvlc) {
		printf("LibVLC initialization failure.\n");
		return EXIT_FAILURE;
	}

	// m = libvlc_media_new_path(libvlc, argv[1]);
	m = libvlc_media_new_location(libvlc, argv[1]);
	mp = libvlc_media_player_new_from_media(m);
	libvlc_media_release(m);

	libvlc_video_set_callbacks(mp, lock, unlock, display, &context);
	libvlc_video_set_format(mp, "RV16", VIDEOWIDTH, VIDEOHEIGHT,
	                        VIDEOWIDTH * 2);
	libvlc_media_player_play(mp);

	// Main loop.
	while (!done) {
		action = 0;
		// Keys: enter (fullscreen), space (pause), escape (quit).
		while (SDL_PollEvent(&event)) {

			switch (event.type) {
			case SDL_QUIT:
				done = 1;
				break;
			case SDL_KEYDOWN:
				action = event.key.keysym.sym;
				break;
			}
		}
		switch (action) {
		case SDLK_ESCAPE:
		case SDLK_q:
			done = 1;
			break;
		case ' ':
			printf("Pause toggle.\n");
			pause = !pause;
			break;
		}

		SDL_Delay(1000 / 10);
	}
	// Stop stream and clean up libVLC.
	libvlc_media_player_stop(mp);
	libvlc_media_player_release(mp);
	libvlc_release(libvlc);
	// Close window and clean up libSDL.
	SDL_DestroyMutex(context.mutex);
	SDL_DestroyRenderer(context.renderer);
	quit(0);
	return 0;
}
