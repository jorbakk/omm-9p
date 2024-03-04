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
	int options = SDL_WINDOW_RESIZABLE;
	if (fullscreen) options = SDL_WINDOW_FULLSCREEN_DESKTOP;
	// create a window with the specified position, dimensions, and flags.
	rctx->sdl_window = SDL_CreateWindow(
		"OMM Renderer",
		SDL_WINDOWPOS_UNDEFINED,
		SDL_WINDOWPOS_UNDEFINED,
		requested_window_width,
		requested_window_height,
		options
		);
	if (rctx->sdl_window == nil) {
		LOG("SDL: could not create window");
		return -1;
	}
	/// Create a 2D rendering context for the SDL_Window
	rctx->sdl_renderer = SDL_CreateRenderer(
		rctx->sdl_window,
		-1,
		SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
	if (rctx->sdl_renderer == nil) {
		LOG("SDL: could not create renderer");
		return -1;
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
		"--no-dbus",
		// "--no-audio",
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
	char *vlcpp = getenv("VLC_PLUGIN_PATH");
	if (vlcpp) {
		LOG("VLC plugin path: %s", vlcpp);
	} else {
		LOG("VLC plugin path not set");
	}
	LOG("Setting DBUS_FATAL_WARNINGS to '0' to prevent libvlc from bailing out ...");
	setenv("DBUS_FATAL_WARNINGS", "0", 1);
	// LOG("Setting X display to ':0' to make sure we have a namespace for plan9port ...");
	// setenv("DISPLAY", ":0", 1);
	return 0;
exit:
	return -1;
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
	LOG("blank window.");
	/// Version 4.0.0 and later only
	// libvlc_media_player_signal(rctx->player);
	// libvlc_media_player_unlock(rctx->player);
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
	/// Keep reading commands in this thread, while the player thread
	/// keeps running
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
	/// Immediately go to next state (without being issued by a command)
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
	libvlc_media_player_pause(rctx->player);
	/// Immediately go to next state (without being issued by a command)
	rctx->renderer_state = transitions[CMD_NONE][rctx->renderer_state];
}


void
state_disengage(RendererCtx *rctx)
{
	libvlc_media_player_pause(rctx->player);
	SDL_PauseAudioDevice(rctx->audio_devid, 1);
	/// Immediately go to next state (without being issued by a command)
	rctx->renderer_state = transitions[CMD_NONE][rctx->renderer_state];
}


void
cmd_seek(RendererCtx *rctx, char *arg, int argn)
{
	if (arg == NULL || argn == 0) {
		LOG("seek cmd arg invalid");
		return;
	}
	libvlc_media_player_set_position(rctx->player, atof(arg) / 100.0);
	// libvlc_media_player_set_position(rctx->player, (float)atoi(arg) / 100.0);
	// float val = strtof(arg);
	// libvlc_media_player_set_position(rctx->player, val / 100.0);
}


void
cmd_vol(RendererCtx *rctx, char *arg, int argn)
{
	if (arg == NULL || argn == 0) {
		LOG("vol cmd arg invalid");
		return;
	}
    libvlc_audio_set_volume(rctx->player, atof(arg));
    // libvlc_audio_set_volume(rctx->player, (float)atoi(arg));
}
