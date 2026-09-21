#include "Editor.h"

#include <graphics/utils/UIWidgets.h>
#include <core/Components.h>

void Editor::draw_gui(entt::registry& registry, AssetManager& assetManager, const EngineStats& stats)
{
	draw_performance_panel(stats);
	draw_entity_hierarchy_panel(registry);
	draw_entity_inspector_panel(registry, assetManager);
}

void Editor::process_event(const SDL_Event& e)
{
	ImGui_ImplSDL2_ProcessEvent(&e);
}

bool Editor::want_capture_mouse() const
{
	return ImGui::GetIO().WantCaptureMouse;
}

bool Editor::want_capture_keyboard() const
{
	return ImGui::GetIO().WantCaptureKeyboard;
}

void Editor::draw_entity_hierarchy_panel(entt::registry& registry)
{
	ImGui::Begin("Scene Hierarchy");

	if (ImGui::Button("+ Create Entity")) {
		entt::entity entity = registry.create();
		registry.emplace<TagComponent>(entity, "Entity " + std::to_string(static_cast<uint32_t>(entity)));
		_selectedEntity = entity;
	}

	ImGui::Separator();

	entt::entity entityToDestroy = entt::null;

	auto view = registry.view<TagComponent>();
	for (auto entity : view) {
		auto& tagComp = view.get<TagComponent>(entity);

		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
		if (_selectedEntity == entity) {
			flags |= ImGuiTreeNodeFlags_Selected;
		}

		bool opened = ImGui::TreeNodeEx(
			reinterpret_cast<void*>(static_cast<uint64_t>(static_cast<uint32_t>(entity))),
			flags,
			"%s",
			tagComp.tag.c_str()
		);

		if (ImGui::IsItemClicked()) {
			_selectedEntity = entity;
		}

		if (ImGui::BeginPopupContextItem()) {
			if (ImGui::MenuItem("Destroy Entity")) {
				entityToDestroy = entity;
			}
			ImGui::EndPopup();
		}

		if (opened) {
			ImGui::TreePop();
		}
	}

	// destroy the entity if marked 
	if (entityToDestroy != entt::null && registry.valid(entityToDestroy)) {
		if (_selectedEntity == entityToDestroy) {
			_selectedEntity = entt::null;
		}
		registry.destroy(entityToDestroy);
	}

	if (ImGui::IsMouseDown(0) && ImGui::IsWindowHovered()) {
		_selectedEntity = entt::null;
	}

	ImGui::End();
}

// renders a selectable component if the entity does not already have ComponentT.
// emplaces ComponentT when clicked and closes the popup.
template<typename ComponentT, typename... Args>
void DrawComponentMenuItem(entt::registry& registry, entt::entity entity, std::string_view name, Args&&... args) {
	if (!registry.all_of<ComponentT>(entity)) {
		if (ImGui::MenuItem(name.data())) {
			registry.emplace<ComponentT>(entity, std::forward<Args>(args)...);
			ImGui::CloseCurrentPopup();
		}
	}
}


void Editor::draw_entity_inspector_panel(entt::registry& registry, AssetManager& assetManager) {
	ImGui::Begin("Inspector");

	if (_selectedEntity == entt::null || !registry.valid(_selectedEntity)) {
		ImGui::TextDisabled("No entity selected.");
		ImGui::End();
		return;
	}

	// tag Component 
	if (registry.all_of<TagComponent>(_selectedEntity)) {
		auto& tag = registry.get<TagComponent>(_selectedEntity).tag;
		std::array<char, 256> buffer{};
		strncpy(buffer.data(), tag.c_str(), sizeof(buffer) - 1);
		if (ImGui::InputText("Tag", buffer.data(), sizeof(buffer))) {
			tag = std::string(buffer.data());
		}
	}
	ImGui::Separator();

	// add domponent dropdown 
	if (ImGui::Button("Add Component")) {
		ImGui::OpenPopup("AddComponentPopup");
	}

	if (ImGui::BeginPopup("AddComponentPopup")) {
		DrawComponentMenuItem<TransformComponent>(registry, _selectedEntity, "Transform");
		DrawComponentMenuItem<MeshComponent>(registry, _selectedEntity, "Mesh");

		ImGui::EndPopup();
	}

	ImGui::Separator();

	// component inspectors 

	// transform component
	if (registry.all_of<TransformComponent>(_selectedEntity)) {
		if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
			auto& tc = registry.get<TransformComponent>(_selectedEntity);
			UIWidgets::DrawVec3Control("Position", tc.position, glm::vec3(0.0f), 0.1f);
			UIWidgets::DrawVec3Control("Rotation", tc.rotation, glm::vec3(0.0f), 0.5f);
			UIWidgets::DrawVec3Control("Scale", tc.scale, glm::vec3(1.0f), 0.05f, &_lockScale);

			if (ImGui::Button("Remove Transform")) {
				registry.remove<TransformComponent>(_selectedEntity);
			}
		}
		ImGui::Separator();
	}

	// mesh component
	if (registry.all_of<MeshComponent>(_selectedEntity)) {
		if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
			auto& mc = registry.get<MeshComponent>(_selectedEntity);
			ImGui::Checkbox("Visible", &mc.visible);

			auto& assets = assetManager.getAssets();
			std::string currentAssetName = "None";
			for (const auto& [name, asset] : assets) {
				if (asset == mc.mesh) {
					currentAssetName = name;
					break;
				}
			}

			if (ImGui::BeginCombo("Scene Asset", currentAssetName.c_str())) {
				bool isNoneSelected = (mc.mesh == nullptr);
				if (ImGui::Selectable("None", isNoneSelected)) {
					mc.mesh = nullptr;
				}
				if (isNoneSelected) {
					ImGui::SetItemDefaultFocus();
				}

				for (const auto& [name, asset] : assets) {
					bool isSelected = (asset == mc.mesh);
					if (ImGui::Selectable(name.c_str(), isSelected)) {
						mc.mesh = asset;
					}
					if (isSelected) {
						ImGui::SetItemDefaultFocus();
					}
				}
				ImGui::EndCombo();
			}

			if (ImGui::Button("Remove Mesh")) {
				registry.remove<MeshComponent>(_selectedEntity);
			}
		}
		ImGui::Separator();
	}

	ImGui::End();
}

void Editor::draw_performance_panel(const EngineStats& stats)
{
	ImGui::Begin("Performance");

	ImGui::Text("Frametime:  %.2f ms", stats.frameTimeMs);
	ImGui::Text("Framerate:  %.1f FPS", stats.fps);
	ImGui::Separator();

	ImGui::Text("Draw Calls:     %d", stats.renderStats.drawcallCount);
	ImGui::Text("Triangles:      %d", stats.renderStats.triangleCount);
	ImGui::Text("Mesh Pass Time: %.2f ms", stats.renderStats.meshDrawTime);

	ImGui::End();
}