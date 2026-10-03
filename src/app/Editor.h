#pragma once

#include "app/editor/EditorPanel.h"
#include <core/Events.h>
#include <core/vk_types.h>
#include <vector>
#include <memory>

class Editor
{
public:
	Editor();

	void draw_gui(entt::registry& registry, AssetManager& assetManager, const EngineStats& stats, const GPUSceneData& sceneData, Renderer& renderer);

	// events
	void on_input_event(const Events::SDLInputEvent& event);
	void on_entity_destroyed(const Events::EntityDestroyedEvent& event);

	// getters
	bool want_capture_mouse() const;
	bool want_capture_keyboard() const;
	bool is_viewport_hovered() const { return _isViewportHovered; }

	template<typename T, typename... Args>
	T* add_panel(Args&&... args) {
		auto panel = std::make_unique<T>(std::forward<Args>(args)...);
		T* ptr = panel.get();
		_panels.push_back(std::move(panel));
		return ptr;
	}

private:
	void draw_main_menu_bar();
	void draw_dockspace();
	void process_viewport_picking(EditorContext& ctx);

	std::vector<std::unique_ptr<EditorPanel>> _panels;
	entt::entity _selectedEntity{ entt::null };

	bool _isViewportHovered{ false };
};