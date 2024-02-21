void
seturl(RendererCtx *rctx, char *url)
{
	(void)rctx;
	LOG("setting url to %s", url);
}


/// VLC prepares to render a video frame.
static void *
vlc_lock(void *data, void **p_pixels)
{
	RendererCtx *rctx = (RendererCtx *)data;
	int pitch;
	SDL_LockMutex(rctx->sdl_mutex);
	SDL_LockTexture(rctx->sdl_texture, NULL, p_pixels, &pitch);
	return NULL;                // Picture identifier, not needed here.
}


/// VLC just rendered a video frame.
static void
vlc_unlock(void *data, void *id, void *const *p_pixels)
{
	(void)id; (void)p_pixels;
	RendererCtx *rctx = (RendererCtx *)data;
	SDL_UnlockTexture(rctx->sdl_texture);
	SDL_UnlockMutex(rctx->sdl_mutex);
}


/// VLC wants to display a video frame.
static void
display(void *data, void *id)
{
	(void)id;
	RendererCtx *rctx = (RendererCtx *)data;
	SDL_Rect rect;
	SDL_GetWindowSize(rctx->sdl_window, &rect.w, &rect.h);
	rect.x = 0;
	rect.y = 0;
	SDL_SetRenderDrawColor(rctx->sdl_renderer, 0, 80, 0, 255);
	SDL_RenderClear(rctx->sdl_renderer);
	SDL_RenderCopy(rctx->sdl_renderer, rctx->sdl_texture, NULL, &rect);
	SDL_RenderPresent(rctx->sdl_renderer);
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
	char const *vlc_argv[] = {
		// "--no-xlib",            // Don't use Xlib.
		"-v",
	};
	int vlc_argc = sizeof(vlc_argv) / sizeof(*vlc_argv);
	rctx->libvlc = libvlc_new(vlc_argc, vlc_argv);
	if (rctx->libvlc == NULL) {
		LOG("LibVLC initialization failure");
		goto exit;
	}
	rctx->sdl_mutex = SDL_CreateMutex();
	LOG("libvlc loading url: %s", rctx->url);
	rctx->media = libvlc_media_new_location(rctx->libvlc, rctx->url);
	// LOG("libvlc loading url: %s", rctx->fileservername);
	// rctx->media = libvlc_media_new_location(rctx->libvlc, rctx->fileservername);
	rctx->player = libvlc_media_player_new_from_media(rctx->media);
	libvlc_media_release(rctx->media);
	libvlc_video_set_callbacks(rctx->player, vlc_lock, vlc_unlock, display, rctx);
	libvlc_video_set_format(rctx->player, "RV16", rctx->w, rctx->h, rctx->w * 2);
	libvlc_media_player_play(rctx->player);
exit:
	rctx->renderer_state = transitions[CMD_NONE][rctx->renderer_state];
}


void
state_unload(RendererCtx *rctx)
{
    libvlc_media_player_stop(rctx->player);
	libvlc_media_player_release(rctx->player);
	libvlc_release(rctx->libvlc);
	SDL_DestroyMutex(rctx->sdl_mutex);
	SDL_DestroyRenderer(rctx->sdl_renderer);
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


int
resize_video(RendererCtx *rctx)
{
	SDL_GetWindowSize(rctx->sdl_window, &rctx->w, &rctx->h);
	LOG("resized sdl window to: %dx%d", rctx->w, rctx->h);
	if (rctx->sdl_texture != nil) {
		SDL_DestroyTexture(rctx->sdl_texture);
	}
	rctx->sdl_texture = SDL_CreateTexture(
		rctx->sdl_renderer,
		SDL_PIXELFORMAT_BGR565,
		// SDL_PIXELFORMAT_YV12,
		SDL_TEXTUREACCESS_STREAMING,
		// SDL_TEXTUREACCESS_TARGET,  // fast update w/o locking, can be used as a render target
		// set video size as the dimensions of the texture
		rctx->w,
		rctx->h
		);
	return 0;
}
