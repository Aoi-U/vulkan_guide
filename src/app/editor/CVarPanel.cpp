#include "CVarPanel.h"

#include <core/cvars.h>
#include <imgui.h>

void CVarPanel::on_draw(EditorContext& ctx)
{
	if (!is_open) return;
	ImGui::Begin(get_name(), &is_open);

	CVarSystem::Get()->DrawCVarEditor();
	ImGui::End();
}
