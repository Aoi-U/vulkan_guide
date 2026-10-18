#pragma once

#include <core/layer/ILayer.h>
#include <app/editor/EditorContext.h>
#include <app/editor/GuiRenderer.h>
#include <functional>
#include <vector>

class EditorLayer : public ILayer
{
public:
	using ImguiDrawCallback = std::function<void(EngineContext& ctx, EditorContext& editorCtx)>;

	EditorLayer()
		: ILayer("EditorLayer") {}
	~EditorLayer() override = default;
	
	void on_attach(EngineContext& ctx) override;
	void on_detach() override;

	void on_begin_frame(uint64_t frameNumber) override;
	void on_update(float deltaTime) override;
	void on_imgui_render() override;
	void on_event(EngineEvent& event) override;

	void add_panel_callback(ImguiDrawCallback callback) { _panelCallbacks.push_back(std::move(callback)); }

	GuiRenderer& get_gui_renderer() noexcept { return _guiRenderer; }
	EditorContext& get_editor_context() noexcept { return _editorCtx; }
	const EditorContext& get_editor_context() const noexcept { return _editorCtx; }

	VkDescriptorSet get_scene_color_descriptor() const noexcept { return _sceneColorDescriptor; }
	VkDescriptorSet get_image_descriptor(VkImageView view, VkSampler sampler = VK_NULL_HANDLE, VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	{
		return _guiRenderer.get_image_descriptor(view, sampler, layout);
	}

private:
	GuiRenderer _guiRenderer;
	EditorContext _editorCtx;
	VkDescriptorSet _sceneColorDescriptor{ VK_NULL_HANDLE };
	bool _imguiFrameActive{ false };
	std::vector<ImguiDrawCallback> _panelCallbacks;
};