#include "EditorLayer.h"

#include <core/layer/LayerStack.h>
#include <graphics/Renderer.h>
#include <resources/DefaultResources.h>
#include <window/window.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <ImGuizmo.h>

void EditorLayer::on_attach(EngineContext& ctx)
{
	ILayer::on_attach(ctx);

	_guiRenderer.init(_ctx->window, _ctx->vulkanCore, _ctx->renderer->get_swapchain_format());

	_ctx->renderer->set_swapchain_recreate_callback([this]() {
		_guiRenderer.clear_image_descriptors();
		_sceneColorDescriptor = VK_NULL_HANDLE;
		});

	_ctx->renderer->set_overlay_render_callback([this](VkCommandBuffer cmd, VkImageView swapchainView, VkExtent2D swapchainExtent, VkImageView finalView) {
		if (finalView != VK_NULL_HANDLE && DefaultResources::get().defaultSamplerLinear.is_valid()) {
			_sceneColorDescriptor = _guiRenderer.get_image_descriptor(finalView, DefaultResources::get().defaultSamplerLinear->get_sampler()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}

		if (_imguiFrameActive) {
			_guiRenderer.render(cmd, swapchainView, swapchainExtent);
			_imguiFrameActive = false;
		}
		});
}

void EditorLayer::on_detach()
{
	if (_ctx && _ctx->renderer) {
		_ctx->renderer->set_overlay_render_callback(nullptr);
		_ctx->renderer->set_swapchain_recreate_callback(nullptr);
	}

	_guiRenderer.clear_image_descriptors();
	_guiRenderer.cleanup();
	_sceneColorDescriptor = VK_NULL_HANDLE;
	_imguiFrameActive = false;

	ILayer::on_detach();
}

void EditorLayer::on_begin_frame(uint64_t frameNumber)
{
	if (!_ctx || !_ctx->window || _ctx->window->is_minimized()) {
		_imguiFrameActive = false;
		return;
	}
	_guiRenderer.new_frame();
	_imguiFrameActive = true;
}

void EditorLayer::on_update(float deltaTime)
{
	if (!_imguiFrameActive || !_ctx || !_ctx->layerStack)
		return;

	// call on_imgui_render() for all enabled layers
	_ctx->layerStack->render_imgui();
}

void EditorLayer::on_imgui_render()
{
	for (auto& cb : _panelCallbacks) {
		if (cb && _ctx) {
			cb(*_ctx, _editorCtx);
		}
	}
}

void EditorLayer::on_event(EngineEvent & event)
{
	// forward raw sdl event to imgui
	ImGui_ImplSDL2_ProcessEvent(&event.sdlEvent);

	const ImGuiIO& io = ImGui::GetIO();

	event.wantCaptureMouse = io.WantCaptureMouse || ImGuizmo::IsUsing();
	event.wantCaptureKeyboard = io.WantCaptureKeyboard;

	// block mouse events from camera/scene layers when cursor is outside viewport
	if (event.is_mouse_event() && !_editorCtx.isViewportHovered && event.wantCaptureMouse) {
		event.handled = true;
	}

	// block keyboard events from camera/scene layers when typing into imgui text widget
	if (event.is_keyboard_event() && !_editorCtx.isViewportFocused && event.wantCaptureKeyboard) {
		event.handled = true;
	}
}
