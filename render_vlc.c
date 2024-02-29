void
seturl(RendererCtx *rctx, char *url)
{
	(void)rctx;
	LOG("setting url to %s", url);
}


int
create_sdl_window(RendererCtx *rctx)
{
	SDL_DisplayMode displaymode;
	if (SDL_GetCurrentDisplayMode(0, &displaymode) != 0) {
		LOG("failed to get sdl display mode");
		return -1;
	}
	rctx->screen_width  = displaymode.w;
	rctx->screen_height = displaymode.h;
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
			// SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE
			SDL_WINDOW_RESIZABLE
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


uint32_t
get_sdl_window_id(SDL_Window *window)
{
	SDL_SysWMinfo wmInfo;
	SDL_VERSION(&wmInfo.version);
	SDL_GetWindowWMInfo(window, &wmInfo);
	return wmInfo.info.x11.window;
}


int
create_window(RendererCtx *rctx)
{
	create_sdl_window(rctx);
	char const *vlc_argv[] = {
		"-v",
	};
	int vlc_argc = sizeof(vlc_argv) / sizeof(*vlc_argv);
	rctx->libvlc = libvlc_new(vlc_argc, vlc_argv);
	if (rctx->libvlc == NULL) {
		LOG("LibVLC initialization failure");
		goto exit;
	}
	rctx->player = libvlc_media_player_new(rctx->libvlc);
	LOG("SDL window id: %u", get_sdl_window_id(rctx->sdl_window));
	libvlc_media_player_set_xwindow(rctx->player, get_sdl_window_id(rctx->sdl_window));
	return 1;
exit:
	return 0;
}


void
close_window(RendererCtx *rctx)
{
	libvlc_media_player_release(rctx->player);
	libvlc_release(rctx->libvlc);
}


void
blank_window(RendererCtx *rctx)
{
	SDL_SetRenderDrawColor(rctx->sdl_renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
	SDL_RenderClear(rctx->sdl_renderer);
	SDL_RenderPresent(rctx->sdl_renderer);
}


int
resize_video(RendererCtx *rctx)
{
	// SDL_GetWindowSize(rctx->sdl_window, &rctx->w, &rctx->h);
	// LOG("resized sdl window to: %dx%d", rctx->w, rctx->h);
	return 0;
}


void
wait_for_window_resize(RendererCtx *rctx)
{
	(void)rctx;
}


void
state_run(RendererCtx *rctx)
{
	while (read_cmd(rctx, READCMD_BLOCK) == KEEP_STATE) {
	}
}


void
state_load(RendererCtx *rctx)
{
	LOG("libvlc loading url: %s", rctx->url);
	rctx->media = libvlc_media_new_location(rctx->libvlc, rctx->url);
	libvlc_media_player_set_media(rctx->player, rctx->media);
	libvlc_media_release(rctx->media);
	/// Start to play ...
	libvlc_media_player_play(rctx->player);
	rctx->renderer_state = transitions[CMD_NONE][rctx->renderer_state];
}


void
state_unload(RendererCtx *rctx)
{
    libvlc_media_player_stop(rctx->player);
	SDL_CloseAudioDevice(rctx->audio_devid);
	// Unconditional transition to STOP state
	rctx->renderer_state = transitions[CMD_NONE][rctx->renderer_state];
}


void
state_engage(RendererCtx *rctx)
{
	SDL_PauseAudioDevice(rctx->audio_devid, 0);
	rctx->renderer_state = transitions[CMD_NONE][rctx->renderer_state];
}


void
state_disengage(RendererCtx *rctx)
{
	SDL_PauseAudioDevice(rctx->audio_devid, 1);
	rctx->renderer_state = transitions[CMD_NONE][rctx->renderer_state];
}
