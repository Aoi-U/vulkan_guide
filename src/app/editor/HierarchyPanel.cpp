#include "app/editor/HierarchyPanel.h"

#include <core/Components.h>
#include <core/Events.h>
#include <imgui.h>
#include <ImGuizmo.h>
#include <resources/AssetRegistry.h>
#include <resources/ModelSpawner.h>
#include <graphics/Renderer.h>

#include <glm/gtx/matrix_decompose.hpp> 
#include <glm/gtc/quaternion.hpp>

static glm::mat4 compute_world_transform(entt::registry& registry, entt::entity entity) {
  glm::mat4 world(1.0f);
  entt::entity current = entity;
  while (current != entt::null) {
    if (const auto* tc = registry.try_get<TransformComponent>(current)) {
      world = tc->get_transform() * world;
    }
    if (const auto* hc = registry.try_get<HierarchyComponent>(current)) {
      current = hc->parent;
    }
    else {
      current = entt::null;
    }
  }
  return world;
}

static bool is_descendant(entt::registry& registry, entt::entity entity, entt::entity potentialDescendant) {
  if (entity == potentialDescendant) return true;
  if (auto* hc = registry.try_get<HierarchyComponent>(entity)) {
    for (auto child : hc->children) {
      if (is_descendant(registry, child, potentialDescendant)) return true;
    }
  }
  return false;
}

static void reparent_entity(entt::registry& registry, entt::entity child, entt::entity newParent) {
  if (child == newParent) return;
  if (newParent != entt::null && is_descendant(registry, child, newParent)) return; // Prevent cyclic loops

  if (!registry.all_of<HierarchyComponent>(child)) registry.emplace<HierarchyComponent>(child, entt::null);
  if (newParent != entt::null && !registry.all_of<HierarchyComponent>(newParent)) registry.emplace<HierarchyComponent>(newParent, entt::null);

  entt::entity oldParent = registry.get<HierarchyComponent>(child).parent;
  if (oldParent == newParent) return;

  if (auto* childTc = registry.try_get<TransformComponent>(child)) {
    glm::mat4 childWorld = childTc->worldTransform;
    glm::mat4 newParentWorld(1.0f);
    if (newParent != entt::null) {
      newParentWorld = compute_world_transform(registry, newParent);
    }

    glm::mat4 newLocal = glm::inverse(newParentWorld) * childWorld;

    glm::vec3 scale;
    glm::quat rotation;
    glm::vec3 translation;
    glm::vec3 skew;
    glm::vec4 perspective;
    glm::decompose(newLocal, scale, rotation, translation, skew, perspective);

    childTc->position = translation;
    childTc->rotation = glm::degrees(glm::eulerAngles(rotation));
    childTc->scale = scale;
  }

  // sever old link
  if (oldParent != entt::null) {
    auto& oldParentHc = registry.get<HierarchyComponent>(oldParent);
    oldParentHc.children.erase(
      std::remove(oldParentHc.children.begin(), oldParentHc.children.end(), child),
      oldParentHc.children.end()
    );
  }

  // make new link
  registry.get<HierarchyComponent>(child).parent = newParent;
  if (newParent != entt::null) {
    registry.get<HierarchyComponent>(newParent).children.push_back(child);
  }
}

static void draw_entity_node(entt::registry& registry, entt::entity entity, EditorContext& ctx, entt::entity& entityToDestroy) {
  auto& tagComp = registry.get<TagComponent>(entity);
  auto* hc = registry.try_get<HierarchyComponent>(entity);

  bool hasChildren = hc && !hc->children.empty();
  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | (ctx.selectedEntity == entity ? ImGuiTreeNodeFlags_Selected : 0);

  if (hasChildren) {
    flags |= ImGuiTreeNodeFlags_OpenOnArrow;
  }
  else {
    flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
  }

  bool opened = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uint64_t>(static_cast<uint32_t>(entity))), flags, "%s", tagComp.tag.c_str());
  if (ImGui::IsItemClicked()) {
    ctx.selectedEntity = entity;
  }

  if (ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload("HIERARCHY_ENTITY", &entity, sizeof(entt::entity));
    ImGui::Text("%s", tagComp.tag.c_str());
    ImGui::EndDragDropSource();
  }
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
      entt::entity droppedEntity = *(const entt::entity*)payload->Data;
      reparent_entity(registry, droppedEntity, entity);
    }
    ImGui::EndDragDropTarget();
  }

  if (ImGui::BeginPopupContextItem()) {
    if (ImGui::MenuItem("Destroy Entity")) {
      entityToDestroy = entity;
    }
    ImGui::EndPopup();
  }

  if (opened) {
    if (hasChildren) {
      for (auto child : hc->children) {
        if (registry.valid(child)) {
          draw_entity_node(registry, child, ctx, entityToDestroy);
        }
      }
    ImGui::TreePop();
    }
  }
}

static void destroy_entity_recursive(entt::registry& registry, entt::entity entity) {
  if (auto* hc = registry.try_get<HierarchyComponent>(entity)) {
    std::vector<entt::entity> childrenCopy = hc->children;
    for (auto child : childrenCopy) {
      if (registry.valid(child)) {
        destroy_entity_recursive(registry, child);
      }
    }
  }
  registry.destroy(entity);
}

void HierarchyPanel::on_draw(EditorContext & ctx)
{
  if (!is_open) return;
  ImGui::Begin(get_name(), &is_open);

  if (ImGui::Button("+ Create Entity")) {
    entt::entity entity = ctx.registry.create();
    ctx.registry.emplace<TagComponent>(entity, "Entity " + std::to_string(static_cast<uint32_t>(entity)));
    ctx.selectedEntity = entity;
    ctx.renderer.mark_scene_dirty();
  }
  ImGui::Separator();

  entt::entity entityToDestroy = entt::null;
  auto view = ctx.registry.view<TagComponent>();

  for (auto entity : view) {
    auto* hc = ctx.registry.try_get<HierarchyComponent>(entity);
    // only draw root entities in the flat loop; children are rendered recursively
    if (!hc || hc->parent == entt::null) {
      draw_entity_node(ctx.registry, entity, ctx, entityToDestroy);
    }
  }

  // invisible dummy to make empty space dropfriendly
  ImGui::Dummy(ImGui::GetContentRegionAvail());
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_MODEL_ID")) {
      std::string draggedId = (const char*)payload->Data;
      auto assetRec = AssetRegistry::get().find_asset_by_id(draggedId);
      if (assetRec) {
        ctx.selectedEntity = spawn_model(ctx.registry, ctx.resourceManager, draggedId, assetRec->path);
        ctx.renderer.mark_scene_dirty();
      }
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
      entt::entity droppedEntity = *(const entt::entity*)payload->Data;
      reparent_entity(ctx.registry, droppedEntity, entt::null);
    }
    // ---------------------------
    ImGui::EndDragDropTarget();
  }

  if (entityToDestroy != entt::null && ctx.registry.valid(entityToDestroy)) {
    if (auto* hc = ctx.registry.try_get<HierarchyComponent>(entityToDestroy)) {
      if (hc->parent != entt::null && ctx.registry.valid(hc->parent)) {
        if (auto* parentHc = ctx.registry.try_get<HierarchyComponent>(hc->parent)) {
          parentHc->children.erase(
            std::remove(parentHc->children.begin(), parentHc->children.end(), entityToDestroy),
            parentHc->children.end()
          );
        }
      }
    }
    destroy_entity_recursive(ctx.registry, entityToDestroy);
    if (ctx.selectedEntity == entityToDestroy) 
      ctx.selectedEntity = entt::null;
		ctx.renderer.mark_scene_dirty();
  }

  if (ImGui::IsMouseDown(0) && ImGui::IsWindowHovered() && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing()) {
    ctx.selectedEntity = entt::null;
  }

  ImGui::End();
}
