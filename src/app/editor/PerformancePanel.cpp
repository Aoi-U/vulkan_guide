#include "PerformancePanel.h"
#include <imgui.h>
#include <graphics/Renderer.h>
#include <core/cvars.h>

void PerformancePanel::on_draw(EditorContext& ctx)
{
  if (!is_open) return;
  ImGui::Begin(get_name(), &is_open);

  // render path switcher
  int currentPath = cvRenderPath.Get();
  const char* pathNames[] = { "Forward Rendering", "Deferred Rendering" };
  if (ImGui::Combo("Render Path", &currentPath, pathNames, 2)) {
    cvRenderPath.Set(currentPath);
  }
  ImGui::Separator();

  ImGui::Text("Frametime:  %.2f ms", ctx.stats.frameTimeMs);
  ImGui::Text("Framerate:  %.1f FPS", ctx.stats.fps);
  ImGui::Separator();

  for (const auto& passStats : ctx.stats.renderStats) {
    ImGui::TextColored(ImVec4(0.3f, 0.7f, 1.0f, 1.0f), "%s", passStats.name.c_str());
    ImGui::Text("  Pass Time: %.2f ms", passStats.passTimeMs);
  }
  ImGui::End();
}
