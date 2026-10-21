#pragma once

#include <core/layer/ILayer.h>
#include <app/editor/EditorContext.h>
#include <app/editor/ContentBrowserPanel.h>
#include <app/editor/InspectorPanel.h>
#include <app/editor/GuiRenderer.h>
#include <scene/SceneSerializer.h>
#include <functional>
#include <vector>
#include <deque>

#include "Events.h"

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
	

private:
	void draw_dockspace_and_menubar();
	void draw_viewport_panel();
	void draw_gizmos(const glm::vec2& viewportPos, const glm::vec2& viewportSize);
	void draw_hierarchy_panel();
	void draw_entity_node(entt::entity entity);
	void draw_render_target_viewer_panel();
	void draw_performance_panel();
	void draw_renderer_settings_panel();

	void delete_entity_recursive(entt::entity entity, bool detachFromParent = true);
	entt::entity pick_entity_at_mouse(const glm::vec2& mousePos, const glm::vec2& viewportPos, const glm::vec2& viewportSize);

	void open_save_scene_as_popup();
	void draw_save_scene_as_popup();
	void on_save_as(const Events::SceneSaveAsEvent&);

	GuiRenderer _guiRenderer;
	EditorContext _editorCtx;
	ContentBrowserPanel _contentBrowserPanel;
	InspectorPanel _inspectorPanel;
	SceneEnvironmentSettings _sceneEnvSettings{};

	VkDescriptorSet _sceneColorDescriptor{ VK_NULL_HANDLE };
	bool _imguiFrameActive{ false };
	std::vector<ImguiDrawCallback> _panelCallbacks;

	std::deque<float> _frameTimeHistory;
	static constexpr size_t MAX_FRAME_HISTORY = 120;

	bool _openSaveAsPopup{ false };
	char _saveAsSceneNameBuf[128]{ "New Scene" };
	char _saveAsFileNameBuf[128]{ "new_scene.scene" };
};
