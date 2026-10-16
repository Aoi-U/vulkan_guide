#include "ViewportPanel.h"

#include <graphics/Renderer.h>
#include <imgui.h>
#include <ImGuizmo.h>
#include <algorithm>
#include <resources/ModelSpawner.h>
#include <resources/AssetRegistry.h>

void ViewportPanel::on_draw(EditorContext& ctx)
{
  if (!is_open) {
    ctx.isViewportHovered = false;
    ctx.isViewportFocused = false;
    ctx.viewportSize = ImVec2(0.0f, 0.0f);
    return;
  }

  // remove window padding around viewport texture
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::Begin(get_name(), &is_open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

  _isHovered = ImGui::IsWindowHovered();
  _isFocused = ImGui::IsWindowFocused();

  ImVec2 avail = ImGui::GetContentRegionAvail();
  ImVec2 cursorScreenPos = ImGui::GetCursorScreenPos();

  _viewportPos = cursorScreenPos;
  _viewportSize = avail;

  ctx.viewportPos = _viewportPos;
  ctx.viewportSize = _viewportSize;
  ctx.isViewportHovered = _isHovered;
  ctx.isViewportFocused = _isFocused;

  // update renderer viewport 
  if (avail.x > 0.0f && avail.y > 0.0f) {
    uint32_t newW = static_cast<uint32_t>(avail.x);
    uint32_t newH = static_cast<uint32_t>(avail.y);
    ctx.renderer.set_viewport_extent({ newW, newH });
  }

  // render scene color texture into imgui window
  VkDescriptorSet textureID = ctx.renderer.get_scene_color_descriptor();
  if (textureID != VK_NULL_HANDLE && avail.x > 0.0f && avail.y > 0.0f) {
    ImGui::Image(reinterpret_cast<ImTextureID>(textureID), avail, ImVec2(0, 0), ImVec2(1, 1));
  }
  else {
    ImGui::Dummy(avail);
  }

  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_MODEL_ID")) {
      std::string draggedId = (const char*)payload->Data;
      auto assetRec = AssetRegistry::get().find_asset_by_id(draggedId);
      if (assetRec) {
        // spawn model at local origin when dropped into viewport
        ctx.selectedEntity = spawn_prefab(ctx.registry, ctx.resourceManager, assetRec->path, glm::vec3(0.0f)); 
        ctx.renderer.mark_scene_dirty();
      }
    }
    ImGui::EndDragDropTarget();
  }
  
  ImGui::End();
  ImGui::PopStyleVar();
}