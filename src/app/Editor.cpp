#include "app/Editor.h"

#include "app/editor/HierarchyPanel.h"
#include "app/editor/InspectorPanel.h"
#include "app/editor/EnvironmentPanel.h"
#include "app/editor/PerformancePanel.h"
#include "app/editor/CVarPanel.h"
#include <imgui_impl_sdl2.h>

Editor::Editor()
{
	add_panel<HierarchyPanel>();
	add_panel<InspectorPanel>();
	add_panel<EnvironmentPanel>();
	add_panel<PerformancePanel>();
	add_panel<CVarPanel>();
}

void Editor::draw_gui(entt::registry& registry, AssetManager& assetManager, const EngineStats& stats)
{
	draw_main_menu_bar();

	EditorContext ctx{
		.registry = registry,
		.assetManager = assetManager,
		.stats = stats,
		.selectedEntity = _selectedEntity
	};

	for (auto& panel : _panels) {
		panel->on_draw(ctx);
	}
}

void Editor::on_input_event(const Events::SDLInputEvent& event)
{
	ImGui_ImplSDL2_ProcessEvent(&event.raw);
}

void Editor::on_entity_destroyed(const Events::EntityDestroyedEvent& event)
{
	if (_selectedEntity == event.entity) {
		_selectedEntity = entt::null;
	}
}

bool Editor::want_capture_mouse() const
{
	return ImGui::GetIO().WantCaptureMouse;
}

bool Editor::want_capture_keyboard() const
{
	return ImGui::GetIO().WantCaptureKeyboard;
}

void Editor::draw_main_menu_bar()
{
	if (ImGui::BeginMainMenuBar()) {
		if (ImGui::BeginMenu("Window")) {
			for (auto& panel : _panels) {
				ImGui::MenuItem(panel->get_name(), nullptr, &panel->is_open);
			}
			ImGui::EndMenu();
		}
		ImGui::EndMainMenuBar();
	}
}
