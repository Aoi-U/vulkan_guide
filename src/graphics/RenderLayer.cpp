#include "RenderLayer.h"

#include <graphics/Renderer.h>
#include <window/window.h>
#include <core/Profiler.h>

void RenderLayer::on_attach(EngineContext& ctx)
{
	ILayer::on_attach(ctx);

	if (_ctx->renderer && _ctx->window && _ctx->is_standalone_mode()) {
		// render at windows native resolution and blit directly to swapchain
		_ctx->renderer->set_overlay_render_callback(nullptr);
		_ctx->renderer->set_viewport_extent(_ctx->window->get_extent());
	}
}

void RenderLayer::on_update(float deltaTime)
{
	if (!_ctx || !_ctx->renderer || !_ctx->window)
		return;

	// keep render extent matched to window size when running without an editor 
	if (_ctx->is_standalone_mode() && !_ctx->window->is_minimized()) {
		_ctx->renderer->set_viewport_extent(_ctx->window->get_extent());
	}
}

void RenderLayer::on_end_frame()
{
	if (!_ctx || !_ctx->renderer || !_ctx->window)
		return;

	if (_ctx->window->is_minimized())
		return;

	if (_ctx->activeRegistry && _ctx->activeSceneData) {
		_ctx->renderer->render_frame(*_ctx->activeRegistry, *_ctx->activeSceneData);
	}
}
