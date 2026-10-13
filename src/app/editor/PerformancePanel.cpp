#include "PerformancePanel.h"
#include <imgui.h>
#include <graphics/Renderer.h>
#include <core/cvars.h>
#include <core/Profiler.h>

void PerformancePanel::on_draw(EditorContext& ctx)
{
  if (!is_open) return;
  ImGui::Begin(get_name(), &is_open);

  Profiler::get().draw_imgui();

  ImGui::End();
}
