#include "PerformancePanel.h"
#include <imgui.h>

void PerformancePanel::on_draw(EditorContext& ctx)
{
	if (!is_open) return;
	ImGui::Begin(get_name(), &is_open);

	ImGui::Text("Frametime:  %.2f ms", ctx.stats.frameTimeMs);
	ImGui::Text("Framerate:  %.1f FPS", ctx.stats.fps);
	ImGui::Separator();


	//ImGui::Text("Draw Calls:     %d", stats.renderStats.drawcallCount);
	//ImGui::Text("Triangles:      %d", stats.renderStats.triangleCount);
	//ImGui::Text("Mesh Pass Time: %.2f ms", stats.renderStats.passTime);

	for (const auto& passStats : ctx.stats.renderStats) {
		ImGui::TextColored(ImVec4(0.3f, 0.7f, 1.0f, 1.0f), "%s", passStats.name.c_str());
		ImGui::Text("  Pass Time: %.2f ms", passStats.passTimeMs);

		for (const auto& metric : passStats.metrics) {
			std::visit([&](const auto& val) {
				using T = std::decay_t<decltype(val)>;
				if constexpr (std::is_same_v<T, uint32_t> || std::is_same_v<T, int32_t>) {
					if (metric.unit.empty()) {
						ImGui::Text("  %s: %d", metric.name.c_str(), val);
					}
					else {
						ImGui::Text("  %s: %d %s", metric.name.c_str(), val, metric.unit.c_str());
					}
				}
				else if constexpr (std::is_same_v<T, float>) {
					if (metric.unit.empty()) {
						ImGui::Text("  %s: %.2f", metric.name.c_str(), val);
					}
					else {
						ImGui::Text("  %s: %.2f %s", metric.name.c_str(), val, metric.unit.c_str());
					}
				}
				else if constexpr (std::is_same_v<T, std::string>) {
					ImGui::Text("  %s: %s", metric.name.c_str(), val.c_str());
				}
				}, metric.value);
		}
		ImGui::Separator();
	}

	ImGui::End();
}
