#pragma once

#include <entt.hpp>
#include <resources/AssetManager.h>
//#include <graphics/MeshRenderer.h>
#include <graphics/pass/ForwardPass.h>
#include <imgui_impl_sdl2.h>


struct EngineStats {
	float frameTimeMs{ 0.0f };
	float fps{ 0.0f };
	//RenderStats renderStats;
	std::vector<PassStats> renderStats;
};

class Editor
{
public:
	void draw_gui(entt::registry& registry, AssetManager& assetManage, const EngineStats& stats);

	entt::entity get_selected_entity() const { return _selectedEntity; }
	void set_selected_entity(entt::entity entity) { _selectedEntity = entity; }

	void process_event(const SDL_Event& e);

	bool want_capture_mouse() const;

	bool want_capture_keyboard() const;

private:
	void draw_performance_panel(const EngineStats& stats);
	void draw_entity_hierarchy_panel(entt::registry& registry);
	void draw_entity_inspector_panel(entt::registry& registry, AssetManager& assetManager);


	entt::entity _selectedEntity{ entt::null };
	bool _lockScale{ true };
};