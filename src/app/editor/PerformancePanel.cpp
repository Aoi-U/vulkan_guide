#include "PerformancePanel.h"
#include <imgui.h>
#include <graphics/Renderer.h>
#include <core/cvars.h>

void PerformancePanel::on_draw(EditorContext& ctx)
{
  if (!is_open) return;
  ImGui::Begin(get_name(), &is_open);

  ImGui::Text("Frametime:  %.2f ms", ctx.stats.frameTimeMs);
  ImGui::Text("Framerate:  %.1f FPS", ctx.stats.fps);

  VkExtent2D vpExt = ctx.renderer.get_viewport_extent();
  VkExtent2D drawExt = ctx.renderer.get_draw_extent();
  float currentPercent = (vpExt.width > 0) ? ((float)drawExt.width / (float)vpExt.width) * 100.0f : 100.0f;
  ImGui::Text("Viewport:    %u x %u", vpExt.width, vpExt.height);
  ImGui::Text("Render Res:  %u x %u (%.0f%%)", drawExt.width, drawExt.height, currentPercent);

  ImGui::Separator();

  for (const auto& passStats : ctx.stats.renderStats) {
    ImGui::TextColored(ImVec4(0.3f, 0.7f, 1.0f, 1.0f), "%s", passStats.name.c_str());
    ImGui::Text("  Pass Time: %.2f ms", passStats.passTimeMs);

    // Unpack and display the custom metrics
    for (const auto& metric : passStats.metrics) {
      if (std::holds_alternative<uint32_t>(metric.value)) {
        ImGui::Text("  %s: %u %s", metric.name.c_str(), std::get<uint32_t>(metric.value), metric.unit.c_str());
      }
      else if (std::holds_alternative<int32_t>(metric.value)) {
        ImGui::Text("  %s: %d %s", metric.name.c_str(), std::get<int32_t>(metric.value), metric.unit.c_str());
      }
      else if (std::holds_alternative<float>(metric.value)) {
        ImGui::Text("  %s: %.2f %s", metric.name.c_str(), std::get<float>(metric.value), metric.unit.c_str());
      }
      else if (std::holds_alternative<std::string>(metric.value)) {
        ImGui::Text("  %s: %s %s", metric.name.c_str(), std::get<std::string>(metric.value).c_str(), metric.unit.c_str());
      }
    }
  }

  ImGui::End();
}
