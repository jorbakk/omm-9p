void
seturl(RendererCtx *rctx, char *url)
{
	(void)rctx;
	LOG("setting url to %s", url);
}

#if 0
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
	// SDL_Rect rect;
	// SDL_GetWindowSize(rctx->sdl_window, &rect.w, &rect.h);
	// rect.x = 0;
	// rect.y = 0;
	SDL_SetRenderDrawColor(rctx->sdl_renderer, 0, 0, 0, 255);
	SDL_RenderClear(rctx->sdl_renderer);
	// SDL_RenderCopy(rctx->sdl_renderer, rctx->sdl_texture, NULL, &rect);
	/// Copy the full texture to the entire rendering target
	SDL_RenderCopy(rctx->sdl_renderer, rctx->sdl_texture, NULL, NULL);
	SDL_RenderPresent(rctx->sdl_renderer);
	// msg_Info(rctx->player, "pitch: %d\n", rctx->pitch);
}
#endif


int
create_window(RendererCtx *rctx)
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
	rctx->player = libvlc_media_player_new(rctx->libvlc);
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
	(void)rctx;
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
	unsigned int video_w, video_h;
	if (libvlc_video_get_size(rctx->player, 0, &video_w, &video_h) == -1) {
		LOG("failed to retrieve video size");
	}
	while (read_cmd(rctx, READCMD_BLOCK) == KEEP_STATE) {
	}
}


void
state_load(RendererCtx *rctx)
{
	rctx->sdl_mutex = SDL_CreateMutex();
	LOG("libvlc loading url: %s", rctx->url);
	rctx->media = libvlc_media_new_location(rctx->libvlc, rctx->url);
	// rctx->player = libvlc_media_player_new_from_media(rctx->media);
	libvlc_media_player_set_media(rctx->player, rctx->media);
	libvlc_media_release(rctx->media);
	/// Attach SDL to the VLC player

	/// ... to the SDL window only
	// libvlc_media_player_set_xwindow(rctx->player, SDL_GetWindowID(rctx->sdl_window));

#if 0
	/// ... or render into an SDL texture
	/// image format is RV16, which means BGR565 (5 bits blue, 6 bits green, 5 bits red)
	/// which uses 16 bit per pixel
	/// The SDL texture is created in the same format
	unsigned int video_w, video_h;
	// if (libvlc_video_get_size(rctx->player, 0, &video_w, &video_h) == -1) {
		// LOG("failed to retrieve video size");
	// }
	video_w = 1920;
	video_h = 816;
	LOG("video size: %ux%u", video_w, video_h);
	rctx->sdl_texture = SDL_CreateTexture(
		rctx->sdl_renderer,
		SDL_PIXELFORMAT_BGR565,
		// SDL_PIXELFORMAT_YV12,
		/// FIXME is there a way to use SDL_TEXTUREACCESS_TARGET with libvlc?
		SDL_TEXTUREACCESS_STREAMING,
		// SDL_TEXTUREACCESS_TARGET,  // fast update w/o locking, can be used as a render target
		// set texture size as the dimensions of the video
		video_w,
		video_h
		);
	libvlc_video_set_callbacks(rctx->player, vlc_lock, vlc_unlock, display, rctx);
	libvlc_video_set_format(rctx->player, "RV16", video_w, video_h, video_w * 2);
#endif

	/// Start to play ...
	libvlc_media_player_play(rctx->player);
// exit:
	rctx->renderer_state = transitions[CMD_NONE][rctx->renderer_state];
}


void
state_unload(RendererCtx *rctx)
{
    libvlc_media_player_stop(rctx->player);
#if 0
	SDL_DestroyMutex(rctx->sdl_mutex);
	SDL_DestroyRenderer(rctx->sdl_renderer);
	SDL_DestroyTexture(rctx->sdl_texture);
#endif
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
