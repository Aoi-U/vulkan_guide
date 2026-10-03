#include "app/editor/HierarchyPanel.h"

#include <core/Components.h>
#include <core/Events.h>
#include <imgui.h>

void HierarchyPanel::on_draw(EditorContext & ctx)
{
  if (!is_open) return;
  ImGui::Begin(get_name(), &is_open);

  if (ImGui::Button("+ Create Entity")) {
    entt::entity entity = ctx.registry.create();
    ctx.registry.emplace<TagComponent>(entity, "Entity " + std::to_string(static_cast<uint32_t>(entity)));
    ctx.selectedEntity = entity;
  }
  ImGui::Separator();

  entt::entity entityToDestroy = entt::null;
  auto view = ctx.registry.view<TagComponent>();

  for (auto entity : view) {
    auto& tagComp = view.get<TagComponent>(entity);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (ctx.selectedEntity == entity) flags |= ImGuiTreeNodeFlags_Selected;

    bool opened = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uint64_t>(static_cast<uint32_t>(entity))),
      flags, "%s", tagComp.tag.c_str());

    if (ImGui::IsItemClicked()) ctx.selectedEntity = entity;

    if (ImGui::BeginPopupContextItem()) {
      if (ImGui::MenuItem("Destroy Entity")) entityToDestroy = entity;
      ImGui::EndPopup();
    }

    if (opened) ImGui::TreePop();
  }

  if (entityToDestroy != entt::null && ctx.registry.valid(entityToDestroy)) {
    ctx.registry.ctx().get<entt::dispatcher>().trigger<Events::EntityDestroyedEvent>({ entityToDestroy });
    ctx.registry.destroy(entityToDestroy);
  }

  if (ImGui::IsMouseDown(0) && ImGui::IsWindowHovered()) {
    ctx.selectedEntity = entt::null;
  }

  ImGui::End();
}
