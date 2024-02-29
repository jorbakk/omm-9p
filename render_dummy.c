void
seturl(RendererCtx *rctx, char *url)
{
	(void)rctx;
	LOG("setting url to %s", url);
}


int
create_window(RendererCtx *rctx)
{
	(void)rctx;
	return 1;
}


void
close_window(RendererCtx *rctx)
{
	(void)rctx;
}


void
blank_window(RendererCtx *rctx)
{
	(void)rctx;
}


int
resize_video(RendererCtx *rctx)
{
	(void)rctx;
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
	rctx->renderer_state = transitions[CMD_NONE][rctx->renderer_state];
}


void
state_unload(RendererCtx *rctx)
{
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


void
cmd_seek(RendererCtx *rctx, char *arg, int argn)
{
	(void)rctx; (void)arg; (void)argn;
}


void
cmd_vol(RendererCtx *rctx, char *arg, int argn)
{
	(void)rctx; (void)arg; (void)argn;
}
