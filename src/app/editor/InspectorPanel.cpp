#include "app/editor/InspectorPanel.h"

#include "app/editor/ComponentRegistry.h"
#include <core/Components.h>
#include <resources/Model.h>
#include <graphics/utils/UIWidgets.h>
#include <array>

InspectorPanel::InspectorPanel()
{
	register_engine_components();
}

void InspectorPanel::on_draw(EditorContext& ctx)
{
  if (!is_open) return;

  ImGui::Begin(get_name(), &is_open);
  if (ctx.selectedEntity == entt::null || !ctx.registry.valid(ctx.selectedEntity)) {
    ImGui::TextDisabled("No entity selected.");
    ImGui::End();
    return;
  }

  const auto& entries = ComponentRegistry::get().get_entries();

  // add component dropdown
  if (ImGui::Button("Add Component")) {
    ImGui::OpenPopup("AddComponentPopup");
  }

  if (ImGui::BeginPopup("AddComponentPopup")) {
    for (const auto& entry : entries) {
      if (!entry.has(ctx.registry, ctx.selectedEntity)) {
        if (ImGui::MenuItem(entry.name.c_str())) {
          entry.add(ctx.registry, ctx.selectedEntity);
          ImGui::CloseCurrentPopup();
        }
      }
    }
    ImGui::EndPopup();
  }
  ImGui::Separator();

  // component UI loop with collapsing headers and context removal
  for (const auto& entry : entries) {
    if (!entry.has(ctx.registry, ctx.selectedEntity)) continue;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap;

    bool headerOpen = ImGui::TreeNodeEx(entry.name.c_str(), flags);
    bool shouldRemove = false;

    if (entry.removable && ImGui::BeginPopupContextItem()) {
      if (ImGui::MenuItem("Remove Component")) {
        shouldRemove = true;
      }
      ImGui::EndPopup();
    }

    if (headerOpen) {
      entry.draw(ctx.registry, ctx.selectedEntity, ctx);
      ImGui::TreePop();
      ImGui::Spacing();
    }

    if (shouldRemove) {
      entry.remove(ctx.registry, ctx.selectedEntity);
    }
  }

  ImGui::End();
}

void InspectorPanel::register_engine_components()
{
	auto& reg = ComponentRegistry::get();

	// tag component
	reg.register_component<TagComponent>("Tag", [](TagComponent& tagComp, EditorContext&) {
		std::array<char, 256> buffer{};
		strncpy(buffer.data(), tagComp.tag.c_str(), sizeof(buffer) - 1);
		if (ImGui::InputText("##TagInput", buffer.data(), sizeof(buffer))) {
			tagComp.tag = std::string(buffer.data());
		}
		}, /* removable = */ false);

	// transform component
	reg.register_component<TransformComponent>("Transform", [this](TransformComponent& tc, EditorContext&) {
		UIWidgets::DrawVec3Control("Position", tc.position, glm::vec3(0.0f), 0.1f);
		UIWidgets::DrawVec3Control("Rotation", tc.rotation, glm::vec3(0.0f), 0.5f);
		UIWidgets::DrawVec3Control("Scale", tc.scale, glm::vec3(1.0f), 0.05f, &_lockScale);
		});

	// mesh component
  reg.register_component<MeshComponent>("Mesh", [](MeshComponent& mc, EditorContext& ctx) {
    ImGui::Checkbox("Visible", &mc.visible);
    auto assets = ctx.resourceManager.get_loaded_resources<Model>();

    std::string currentName = "None";
    if (mc.model.is_valid()) currentName = mc.model.get_id();

    if (ImGui::BeginCombo("Scene Asset", currentName.c_str())) {
      if (ImGui::Selectable("None", !mc.model.is_valid())) {
        mc.model = ResourceHandle<Model>();
      }
      for (const auto& name : assets) {
        bool isSelected = (name == currentName);
        if (ImGui::Selectable(name.c_str(), isSelected)) {
          mc.model = ResourceHandle<Model>(name, &ctx.resourceManager);
        }
        if (isSelected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    });

	// directional light component
	reg.register_component<DirectionalLightComponent>("Directional Light", [](DirectionalLightComponent& dl, EditorContext&) {
		ImGui::Checkbox("Visible", &dl.visible);
		ImGui::ColorEdit3("Color", &dl.color.x);
		ImGui::DragFloat("Intensity", &dl.intensity, 0.1f, 0.0f, 100.0f, "%.2f");
		UIWidgets::DrawVec3Control("Direction", dl.direction, glm::vec3(0.4f, 1.0f, 0.3f), 0.05f);
		ImGui::DragFloat("Shadow Extent", &dl.shadowExtent, 0.5f, 1.0f, 200.0f, "%.1f");
		});

	// point light component
	reg.register_component<PointLightComponent>("Point Light", [](PointLightComponent& pl, EditorContext&) {
		ImGui::Checkbox("Visible", &pl.visible);
		ImGui::Checkbox("Cast Shadow", &pl.castShadow);
		ImGui::ColorEdit3("Color", &pl.color.x);
		ImGui::DragFloat("Intensity", &pl.intensity, 5.0f, 0.0f, 10000.0f, "%.1f");
		ImGui::DragFloat("Radius", &pl.radius, 0.1f, 0.01f, 100.0f, "%.2f");
		});
}
