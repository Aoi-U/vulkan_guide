#include "ViewportPanel.h"

#include <graphics/Renderer.h>
#include <imgui.h>
#include <ImGuizmo.h>
#include <algorithm>

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

  // resize renderer if panel size changed
  if (avail.x > 0.0f && avail.y > 0.0f) {
    VkExtent2D currentExtent = ctx.renderer.get_viewport_extent();
    uint32_t newW = static_cast<uint32_t>(avail.x);
    uint32_t newH = static_cast<uint32_t>(avail.y);

    if (newW != currentExtent.width || newH != currentExtent.height) {
      ctx.renderer.set_viewport_extent({ newW, newH });
    }
  }

  // render the scene color texture into imgui window
  VkDescriptorSet textureID = ctx.renderer.get_scene_color_descriptor();
  if (textureID != VK_NULL_HANDLE && avail.x > 0.0f && avail.y > 0.0f) {
    ImGui::Image(reinterpret_cast<ImTextureID>(textureID), avail, ImVec2(0, 0), ImVec2(1, 1));
  }

  ImGui::End();
  ImGui::PopStyleVar();
}