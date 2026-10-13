#include "app/editor/RenderTargetViewPanel.h"

#include <graphics/Renderer.h>
#include <imgui.h>
#include <vulkan/vk_enum_string_helper.h>
#include <algorithm>
#include <resources/DefaultResources.h>
#include <graphics/GuiRenderer.h>

void RenderTargetViewPanel::on_draw(EditorContext& ctx)
{
  if (!is_open) return;

  ImGui::Begin(get_name(), &is_open);

  const auto& renderTargets = ctx.renderer.get_debug_targets();
  std::vector<std::string> targetNames;
  for (const auto& [name, texture] : renderTargets) {
    targetNames.push_back(name);
  } 

  if (targetNames.empty()) {
    ImGui::TextDisabled("No render targets registered.");
    ImGui::End();
    return;
  }

  // ensure selected target is valid
  if (std::find(targetNames.begin(), targetNames.end(), _selectedTarget) == targetNames.end()) {
    _selectedTarget = targetNames[0];
  }

  // target selector combo
  if (ImGui::BeginCombo("Target", _selectedTarget.c_str())) {
    for (const auto& name : targetNames) {
      bool isSelected = (_selectedTarget == name);
      if (ImGui::Selectable(name.c_str(), isSelected)) {
        _selectedTarget = name;
      }
      if (isSelected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }

  //const Texture& selectedTex = renderTargets.at(_selectedTarget);
	const TargetInfo& selectedTargetInfo = renderTargets.at(_selectedTarget);
  VkFormat format = selectedTargetInfo.format;
  VkExtent2D currentExtent = selectedTargetInfo.extent;
  bool isCube = selectedTargetInfo.isCube;

  // metadata Bar
  ImGui::Text("Size: %u x %u | Format: %s", currentExtent.width, currentExtent.height, string_VkFormat(format));

  // cubemap face controls
  if (isCube) {
    ImGui::Separator();
    ImGui::Text("Cubemap Face:");
    const char* faceNames[] = { "+X (Right)", "-X (Left)", "+Y (Top)", "-Y (Bottom)", "+Z (Front)", "-Z (Back)" };
    for (int i = 0; i < 6; ++i) {
      if (i > 0) ImGui::SameLine();
      if (ImGui::RadioButton(faceNames[i], _selectedCubeFace == i)) {
        _selectedCubeFace = i;
      }
    }
  }

  // channel hint for specific bguffer attachments
  if (_selectedTarget == "GBufferMetalRough") {
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Channels: [R] Metallic  [G] Roughness  [B] glTF AO");
  }
  else if (_selectedTarget == "GBufferNormal") {
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Channels: [RGB] World Space Normal Vector");
  }
  else if (_selectedTarget == "GBufferPosition") {
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Channels: [RGB] World Position  [A] HasGeometry");
  }
  else if (_selectedTarget == "SSAOColor" || _selectedTarget == "SSAOBlur") {
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Grayscale: Ambient Occlusion factor [0.0 = dark, 1.0 = open]");
  }

  ImGui::Separator();

  // display sizing options
  ImGui::Checkbox("Fit to Window", &_fitToWindow);
  if (!_fitToWindow) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    ImGui::SliderFloat("Zoom", &_zoomScale, 0.1f, 4.0f, "%.2fx");
  }

  // calculate display dimensions 
  ImVec2 avail = ImGui::GetContentRegionAvail();
  ImVec2 displaySize(static_cast<float>(currentExtent.width), static_cast<float>(currentExtent.height));

  if (_fitToWindow) {
    float aspect = displaySize.x / displaySize.y;
    if (avail.x / avail.y > aspect) {
      displaySize.y = std::max(10.0f, avail.y);
      displaySize.x = displaySize.y * aspect;
    }
    else {
      displaySize.x = std::max(10.0f, avail.x);
      displaySize.y = displaySize.x / aspect;
    }
  }
  else {
    displaySize.x *= _zoomScale;
    displaySize.y *= _zoomScale;
  }

  // retrieve descriptor and render
  VkImageView targetView = isCube ? selectedTargetInfo.faceViews[_selectedCubeFace] : selectedTargetInfo.view;
  if (targetView == VK_NULL_HANDLE) {
    ImGui::TextDisabled("Render target view not ready");
    ImGui::End();
    return;
  }

  VkDescriptorSet descriptor = ctx.renderer.get_gui_renderer()->get_image_descriptor(targetView, DefaultResources::get().defaultSamplerLinear->get_sampler());
  if (descriptor != VK_NULL_HANDLE && displaySize.x > 0.0f && displaySize.y > 0.0f) {
    ImGui::BeginChild("##ImageScrollRegion", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::Image(reinterpret_cast<ImTextureID>(descriptor), displaySize, ImVec2(0, 0), ImVec2(1, 1));
    ImGui::EndChild();
  }

  ImGui::End();
}
