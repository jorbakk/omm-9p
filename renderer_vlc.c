void state_run(RendererCtx *rctx)
{
	rctx->quit = 1;
	rctx->renderer_state = transitions[CMD_NONE][rctx->renderer_state];
}


void state_load(RendererCtx *rctx)
{
	rctx->renderer_state = transitions[CMD_NONE][rctx->renderer_state];
}


void state_unload(RendererCtx *rctx)
{
	SDL_CloseAudioDevice(rctx->audio_devid);
	// Unconditional transition to STOP state
	rctx->renderer_state = transitions[CMD_NONE][rctx->renderer_state];
}


void state_engage(RendererCtx *rctx)
{
	SDL_PauseAudioDevice(rctx->audio_devid, 0);
	rctx->renderer_state = transitions[CMD_NONE][rctx->renderer_state];
}


void state_disengage(RendererCtx *rctx)
{
	SDL_PauseAudioDevice(rctx->audio_devid, 1);
	rctx->renderer_state = transitions[CMD_NONE][rctx->renderer_state];
}


int
resize_video(RendererCtx *rctx)
{
	SDL_GetWindowSize(rctx->sdl_window, &rctx->w, &rctx->h);
	LOG("resized sdl window to: %dx%d", rctx->w, rctx->h);
	return 0;
}
