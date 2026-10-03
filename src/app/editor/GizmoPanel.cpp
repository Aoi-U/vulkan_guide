#include "app/editor/GizmoPanel.h"

#include <core/Components.h>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

void GizmoPanel::on_draw(EditorContext& ctx)
{
	if (!is_open) return;
	if (ctx.viewportSize.x <= 0.0f || ctx.viewportSize.y <= 0.0f) return;

	ImGuiIO& io = ImGui::GetIO();
	const bool isFreelooking = ImGui::IsMouseDown(ImGuiMouseButton_Right);

	// process keyboard shortcuts when not typing and not freelooking
	if (!io.WantTextInput && !isFreelooking) {
		if (ImGui::IsKeyPressed(ImGuiKey_W)) _currentOp = ImGuizmo::TRANSLATE;
		if (ImGui::IsKeyPressed(ImGuiKey_E)) _currentOp = ImGuizmo::ROTATE;
		if (ImGui::IsKeyPressed(ImGuiKey_R)) _currentOp = ImGuizmo::SCALE;
	}

	// append to "Viewport" so the controls are in the viewports window hierarchy
	if (ImGui::Begin("Viewport")) {

		// floating toolbar overlay
		ImGui::SetCursorPos(ImVec2(10.0f, 25.0f));

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->ChannelsSplit(2);
		drawList->ChannelsSetCurrent(1); // foreground (buttons)

		ImGui::BeginGroup();
		ImGui::PushID("GizmoToolbar");

		if (ImGui::RadioButton("Translate (W)", _currentOp == ImGuizmo::TRANSLATE)) _currentOp = ImGuizmo::TRANSLATE;
		ImGui::SameLine();
		if (ImGui::RadioButton("Rotate (E)", _currentOp == ImGuizmo::ROTATE)) _currentOp = ImGuizmo::ROTATE;
		ImGui::SameLine();
		if (ImGui::RadioButton("Scale (R)", _currentOp == ImGuizmo::SCALE)) _currentOp = ImGuizmo::SCALE;
		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		if (ImGui::RadioButton("Local", _currentMode == ImGuizmo::LOCAL)) _currentMode = ImGuizmo::LOCAL;
		ImGui::SameLine();
		if (ImGui::RadioButton("World", _currentMode == ImGuizmo::WORLD)) _currentMode = ImGuizmo::WORLD;
		ImGui::SameLine();
		ImGui::Checkbox("Snap", &_useSnap);

		ImGui::PopID();
		ImGui::EndGroup();

		// check if mouse is hovering over the toolbar rectangle
		ImVec2 tbMin = ImGui::GetItemRectMin();
		ImVec2 tbMax = ImGui::GetItemRectMax();
		tbMin.x -= 8.0f; tbMin.y -= 4.0f;
		tbMax.x += 8.0f; tbMax.y += 4.0f;

		ctx.isGizmoControlsHovered = (io.MousePos.x >= tbMin.x && io.MousePos.x <= tbMax.x &&
			io.MousePos.y >= tbMin.y && io.MousePos.y <= tbMax.y);

		// draw background pill behind toolbar
		drawList->ChannelsSetCurrent(0);
		drawList->AddRectFilled(tbMin, tbMax, IM_COL32(20, 20, 24, 220), 4.0f);
		drawList->AddRect(tbMin, tbMax, IM_COL32(60, 60, 72, 255), 4.0f);
		drawList->ChannelsMerge();

		// 3d imguizmo handle manipulation
		if (ctx.selectedEntity != entt::null && ctx.registry.valid(ctx.selectedEntity)) {
			if (auto* tc = ctx.registry.try_get<TransformComponent>(ctx.selectedEntity)) {

				bool enableGizmo = !isFreelooking && !ctx.isGizmoControlsHovered && (ctx.isViewportHovered || ImGuizmo::IsUsing());
				ImGuizmo::Enable(enableGizmo);
				ImGuizmo::SetOrthographic(false);
				ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
				ImGuizmo::SetRect(ctx.viewportPos.x, ctx.viewportPos.y, ctx.viewportSize.x, ctx.viewportSize.y);

				// build opengl right handed perspective projection matrix
				float aspect = ctx.viewportSize.x / ctx.viewportSize.y;
				float tanHalfFov = std::tan(glm::radians(70.0f) * 0.5f);
				float f = 1.0f / tanHalfFov;
				float n = 0.1f;
				float f_dist = 1000.0f;

				glm::mat4 imguizmoProj(0.0f);
				imguizmoProj[0][0] = f / aspect;
				imguizmoProj[1][1] = f;
				imguizmoProj[2][2] = -(f_dist + n) / (f_dist - n);
				imguizmoProj[2][3] = -1.0f;
				imguizmoProj[3][2] = -(2.0f * f_dist * n) / (f_dist - n);

				glm::mat4 view = ctx.sceneData.view;

				// build transform matrix from position, rotation, scale
				glm::mat4 transformMatrix(1.0f);
				ImGuizmo::RecomposeMatrixFromComponents(
					&tc->position.x,
					&tc->rotation.x,
					&tc->scale.x,
					glm::value_ptr(transformMatrix)
				);

				float* snapValues = nullptr;
				if (_useSnap) {
					if (_currentOp == ImGuizmo::TRANSLATE) snapValues = _snapTranslation;
					else if (_currentOp == ImGuizmo::ROTATE) snapValues = &_snapRotation;
					else if (_currentOp == ImGuizmo::SCALE) snapValues = &_snapScale;
				}

				// manipulate the transform matrix using imguizmo
				bool manipulated = ImGuizmo::Manipulate(
					glm::value_ptr(view),
					glm::value_ptr(imguizmoProj),
					_currentOp,
					_currentMode,
					glm::value_ptr(transformMatrix),
					nullptr,
					snapValues
				);

				// decompose transform matrix back into component
				if (manipulated && ImGuizmo::IsUsing()) {
					ImGuizmo::DecomposeMatrixToComponents(
						glm::value_ptr(transformMatrix),
						&tc->position.x,
						&tc->rotation.x,
						&tc->scale.x
					);
				}
			}
		}

		ImGui::End(); 
	}
}
