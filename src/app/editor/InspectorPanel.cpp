#include "app/editor/InspectorPanel.h"

#include "app/editor/ComponentRegistry.h"
#include <resources/AssetRegistry.h>
#include <core/Components.h>
#include <resources/Model.h>
#include <graphics/utils/UIWidgets.h>
#include <graphics/Renderer.h>
#include <glm/gtc/type_ptr.hpp>
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
    if (UIWidgets::DrawVec3Control("Position", tc.position, glm::vec3(0.0f), 0.1f)) 
      tc.isDirty = true;
    if (UIWidgets::DrawVec3Control("Rotation", tc.rotation, glm::vec3(0.0f), 0.5f)) 
      tc.isDirty = true;
    if (UIWidgets::DrawVec3Control("Scale", tc.scale, glm::vec3(1.0f), 0.05f, &_lockScale)) 
      tc.isDirty = true;
		});

	// mesh component
  reg.register_component<MeshComponent>("Mesh", [](MeshComponent& mc, EditorContext& ctx) {
    ImGui::Checkbox("Visible", &mc.visible);

    std::vector<std::string> loadedMeshes = ctx.resourceManager.get_loaded_resources<Mesh>();
    std::string currentMeshId = mc.mesh.is_valid() ? mc.mesh.get_id() : "None";

    if (ImGui::BeginCombo("Mesh Asset", currentMeshId.c_str())) {
      if (ImGui::Selectable("None", !mc.mesh.is_valid())) {
        mc.mesh = ResourceHandle<Mesh>();
        mc.startIndex = 0;
        mc.count = 0;
      }
      for (const auto& meshId : loadedMeshes) {
        bool isSelected = (currentMeshId == meshId);
        if (ImGui::Selectable(meshId.c_str(), isSelected)) {
          mc.mesh = ResourceHandle<Mesh>(meshId, &ctx.resourceManager);
          if (mc.mesh.is_valid()) {
            mc.startIndex = 0;
            mc.count = mc.mesh->get_index_count();
          }
          ctx.renderer.mark_scene_dirty();
        }
        if (isSelected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }

    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_MESH_ID")) {
        std::string draggedId = (const char*)payload->Data;
        mc.mesh = ResourceHandle<Mesh>(draggedId, &ctx.resourceManager);
        if (mc.mesh.is_valid()) {
          mc.startIndex = 0;
          mc.count = mc.mesh->get_index_count();
        }
        ctx.renderer.mark_scene_dirty();
      }
      ImGui::EndDragDropTarget();
    }

    if (mc.mesh.is_valid()) {
      ImGui::Separator();
      ImGui::Text("Vertices: %u", mc.mesh->get_vertex_count());
      ImGui::Text("Device Address: 0x%llx", (unsigned long long)mc.mesh->get_vertex_buffer_address());

      int startIdx = static_cast<int>(mc.startIndex);
      int count = static_cast<int>(mc.count);

      if (ImGui::DragInt("Start Index", &startIdx, 1, 0, mc.mesh->get_index_count())) {
        mc.startIndex = static_cast<uint32_t>(std::max(0, startIdx));
        ctx.renderer.mark_scene_dirty();
      }
      if (ImGui::DragInt("Count", &count, 1, 0, mc.mesh->get_index_count() - mc.startIndex)) {
        mc.count = static_cast<uint32_t>(std::max(0, count));
        ctx.renderer.mark_scene_dirty();
      }

      if (ImGui::Button("Reset to Full Mesh")) {
        mc.startIndex = 0;
        mc.count = mc.mesh->get_index_count();
        ctx.renderer.mark_scene_dirty();
      }

      ImGui::Spacing();
      ImGui::Text("Bounds:");
      ImGui::DragFloat3("Origin", &mc.bounds.origin.x, 0.1f);
      ImGui::DragFloat("Radius", &mc.bounds.sphereRadius, 0.1f);
      ImGui::DragFloat3("Extents", &mc.bounds.extents.x, 0.1f);
    }
    else {
      ImGui::TextDisabled("No geometry attached.");
    }
    });

  // material component
  reg.register_component<MaterialComponent>("Material", [](MaterialComponent& mat, EditorContext& ctx) {
    bool dirty = false;

    if (ImGui::ColorEdit4("Albedo Color", glm::value_ptr(mat.albedoColor))) dirty = true;

    auto draw_tex_selector = [&](const char* label, ResourceHandle<Texture>& tex) {
      ImGui::PushID(label);
      std::string current = tex.is_valid() ? tex.get_id() : "None";
      if (ImGui::BeginCombo(label, current.c_str())) {
        if (ImGui::Selectable("None", !tex.is_valid())) { tex = ResourceHandle<Texture>(); dirty = true; }

        // show dynamically generated / model-embedded textures via ResourceManager
        std::vector<std::string> loadedTextures = ctx.resourceManager.get_loaded_resources<Texture>();
        for (const auto& texName : loadedTextures) {
          if (ImGui::Selectable(texName.c_str(), current == texName)) {
            tex = ResourceHandle<Texture>(texName, &ctx.resourceManager);
            dirty = true;
          }
        }

        // show unloaded files from disk via AssetRegistry
        for (const auto& asset : AssetRegistry::get().get_assets_by_type(AssetType::Texture)) {
          if (std::find(loadedTextures.begin(), loadedTextures.end(), asset.id) == loadedTextures.end()) {
            if (ImGui::Selectable(asset.id.c_str(), current == asset.id)) {
              tex = ctx.resourceManager.load<Texture>(asset.id, asset.path, VK_FORMAT_R8G8B8A8_UNORM);
              dirty = true;
            }
          }
        }
        ImGui::EndCombo();
      }
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TEXTURE_ID")) {
          std::string draggedId = (const char*)payload->Data;
          if (auto assetRec = AssetRegistry::get().find_asset_by_id(draggedId)) {
            tex = ctx.resourceManager.load<Texture>(draggedId, assetRec->path, VK_FORMAT_R8G8B8A8_UNORM);
            dirty = true;
          }
        }
        ImGui::EndDragDropTarget();
      }
      ImGui::PopID();
      };

    draw_tex_selector("Albedo Texture", mat.albedoTexture);
    draw_tex_selector("Metallic-Roughness Texture", mat.metallicRoughnessTexture);
    draw_tex_selector("Normal Map", mat.normalTexture);
    draw_tex_selector("Emissive Texture", mat.emissiveTexture);
    draw_tex_selector("Occlusion Texture", mat.occlusionTexture);

    if (ImGui::SliderFloat("Metallic", &mat.metallic, 0.0f, 1.0f)) dirty = true;
    if (ImGui::SliderFloat("Roughness", &mat.roughness, 0.0f, 1.0f)) dirty = true;
    if (ImGui::SliderFloat("Normal Strength", &mat.normalStrength, 0.0f, 2.0f)) dirty = true;

    if (ImGui::ColorEdit3("Emissive Color", glm::value_ptr(mat.emissiveColor))) dirty = true;
    if (ImGui::DragFloat("Emissive Intensity", &mat.emissiveIntensity, 0.1f, 0.0f, 100.0f, "%.1f")) dirty = true;

    if (ImGui::SliderFloat("Occlusion Strength", &mat.occlusionStrength, 0.0f, 1.0f)) dirty = true;

    const char* alphaModes[] = { "Opaque", "Mask", "Blend" };
    int currentAlpha = static_cast<int>(mat.alphaMode);
    if (ImGui::Combo("Alpha Mode", &currentAlpha, alphaModes, 3)) {
      mat.alphaMode = static_cast<AlphaMode>(currentAlpha);
      dirty = true;
    }

    if (mat.alphaMode == AlphaMode::Mask) {
      if (ImGui::SliderFloat("Alpha Cutoff", &mat.alphaCutoff, 0.0f, 1.0f)) dirty = true;
    }

    if (ImGui::Checkbox("Double Sided", &mat.doubleSided)) dirty = true;

    if (dirty) mat.isDirty = true;
    });

  reg.register_component<HierarchyComponent>("Hierarchy", [](HierarchyComponent& hc, EditorContext& ctx) {
    ImGui::Text("Parent Entity ID: %d", hc.parent != entt::null ? static_cast<uint32_t>(hc.parent) : -1);
    if (ImGui::TreeNode("Children", "Children Count: %zu", hc.children.size())) {
      for (auto child : hc.children) {
        ImGui::BulletText("Entity ID: %d", static_cast<uint32_t>(child));
      }
      ImGui::TreePop();
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
