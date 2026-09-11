#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_vulkan.h"

#include <glm/gtx/transform.hpp>
#include <string>

#include <imgui.h>
#include <glm/glm.hpp>
#include <string>

static void DrawVec3ControlStyled(const char* label,
  glm::vec3& values,
  const glm::vec3& defaultValues = glm::vec3(0.0f),
  float speed = 0.05f,
  bool* pLocked = nullptr)
{
  ImGui::PushID(label);

  // --- Header Row: Label, Reset Button, and Lock Toggle ---
  ImGui::Text("%s", label);
  ImGui::SameLine();

  // Default / Reset Button
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.25f, 0.6f));
  if (ImGui::SmallButton("Reset")) {
    values = defaultValues;
  }
  ImGui::PopStyleColor();
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Reset values to default");
  }

  // Uniform Lock Toggle (if a lock state pointer is provided)
  if (pLocked) {
    ImGui::SameLine();
    if (*pLocked) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.8f, 0.9f));
    }
    else {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.25f, 0.6f));
    }

    if (ImGui::SmallButton(*pLocked ? "Locked" : "Unlocked")) {
      *pLocked = !(*pLocked);
    }
    ImGui::PopStyleColor();

    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Lock axes to change all dimensions uniformly");
    }
  }

  // --- Styling for Navy Blue Input Boxes ---
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.16f, 0.24f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.16f, 0.22f, 0.32f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.20f, 0.26f, 0.38f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));

  // Calculate equal width for all 3 sliders
  float totalWidth = ImGui::GetContentRegionAvail().x;
  float itemSpacing = ImGui::GetStyle().ItemSpacing.x;
  float itemWidth = (totalWidth - (itemSpacing * 2.0f)) / 3.0f;
  float underlineHeight = 2.5f;

  ImDrawList* drawList = ImGui::GetWindowDrawList();
  glm::vec3 prevValues = values;

  // --- Axis Setup Data ---
  const char* ids[3] = { "##X", "##Y", "##Z" };
  float* components[3] = { &values.x, &values.y, &values.z };
  const float* prevComponents[3] = { &prevValues.x, &prevValues.y, &prevValues.z };
  ImU32 colors[3] = {
      IM_COL32(225, 45, 45, 255),   // Red (X)
      IM_COL32(45, 200, 55, 255),   // Green (Y)
      IM_COL32(35, 65, 240, 255)    // Blue (Z)
  };

  for (int i = 0; i < 3; i++) {
    if (i > 0) ImGui::SameLine();

    ImGui::SetNextItemWidth(itemWidth);
    if (ImGui::DragFloat(ids[i], components[i], speed, 0.0f, 0.0f, "%.3f")) {
      // Uniform lock logic
      if (pLocked && *pLocked) {
        float delta = *components[i] - *prevComponents[i];
        for (int j = 0; j < 3; j++) {
          if (j != i) {
            *components[j] += delta;
          }
        }
      }
    }

    // Draw bottom accent line
    ImVec2 rectMin = ImGui::GetItemRectMin();
    ImVec2 rectMax = ImGui::GetItemRectMax();
    drawList->AddRectFilled(
      ImVec2(rectMin.x, rectMax.y - underlineHeight),
      rectMax,
      colors[i],
      ImGui::GetStyle().FrameRounding,
      ImDrawFlags_RoundCornersBottom
    );
  }

  ImGui::PopStyleVar();
  ImGui::PopStyleColor(3);
  ImGui::PopID();
}