#include "app/editor/GizmoPanel.h"

#include <core/Components.h>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

void GizmoPanel::on_draw(EditorContext& ctx)
{
  ImGuiIO& io = ImGui::GetIO();
  const bool isFreelooking = ImGui::IsMouseDown(ImGuiMouseButton_Right);

	// process keyboard shortcuts
  if (!io.WantTextInput && !isFreelooking) {
    if (ImGui::IsKeyPressed(ImGuiKey_W)) _currentOp = ImGuizmo::TRANSLATE;
    if (ImGui::IsKeyPressed(ImGuiKey_E)) _currentOp = ImGuizmo::ROTATE;
    if (ImGui::IsKeyPressed(ImGuiKey_R)) _currentOp = ImGuizmo::SCALE;
  }

  ImGuizmo::Enable(!isFreelooking);

  // draw floating control overlay
  ImGui::SetNextWindowPos(ImVec2(10.0f, 30.0f), ImGuiCond_FirstUseEver);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

  if (ImGui::Begin("##GizmoControls", nullptr, flags)) {
    if (ImGui::RadioButton("Translate (W)", _currentOp == ImGuizmo::TRANSLATE)) _currentOp = ImGuizmo::TRANSLATE;
    ImGui::SameLine();
    if (ImGui::RadioButton("Rotate (E)", _currentOp == ImGuizmo::ROTATE)) _currentOp = ImGuizmo::ROTATE;
    ImGui::SameLine();
    if (ImGui::RadioButton("Scale (R)", _currentOp == ImGuizmo::SCALE)) _currentOp = ImGuizmo::SCALE;

    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();

    if (ImGui::RadioButton("Local", _currentMode == ImGuizmo::LOCAL)) _currentMode = ImGuizmo::LOCAL;
    ImGui::SameLine();
    if (ImGui::RadioButton("World", _currentMode == ImGuizmo::WORLD)) _currentMode = ImGuizmo::WORLD;

    ImGui::SameLine();
    ImGui::Checkbox("Snap", &_useSnap);
  }
  ImGui::End();
  
	// check if there is a selected entity
  if (ctx.selectedEntity == entt::null || !ctx.registry.valid(ctx.selectedEntity)) {
    return;
  }

	// check if the selected entity has a TransformComponent
  auto* tc = ctx.registry.try_get<TransformComponent>(ctx.selectedEntity);
  if (!tc) {
    return;
  }

	// setup ImGuizmo
  ImGuizmo::SetOrthographic(false);
  ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());
  ImGuizmo::SetRect(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);

  // correct projection matrix for imguizmo
  glm::mat4 view = ctx.sceneData.view;
  glm::mat4 proj = ctx.sceneData.proj;
  proj[1][1] *= -1.0f; // Restore OpenGL NDC for ImGuizmo

  // setup transform matrix
  glm::mat4 transformMatrix(1.0f);
  ImGuizmo::RecomposeMatrixFromComponents(
    &tc->position.x,
    &tc->rotation.x,
    &tc->scale.x,
    glm::value_ptr(transformMatrix)
  );

  // determine snap value
  float* snapValues = nullptr;
  if (_useSnap) {
    if (_currentOp == ImGuizmo::TRANSLATE) snapValues = _snapTranslation;
    else if (_currentOp == ImGuizmo::ROTATE) snapValues = &_snapRotation;
    else if (_currentOp == ImGuizmo::SCALE) snapValues = &_snapScale;
  }

  // perform manipulation
  bool manipulated = ImGuizmo::Manipulate(
    glm::value_ptr(view),
    glm::value_ptr(proj),
    _currentOp,
    _currentMode,
    glm::value_ptr(transformMatrix),
    nullptr,
    snapValues
  );

  // update TransformComponent if manipulated
  if (manipulated && ImGuizmo::IsUsing()) {
    ImGuizmo::DecomposeMatrixToComponents(
      glm::value_ptr(transformMatrix),
      &tc->position.x,
      &tc->rotation.x,
      &tc->scale.x
    );
  }
}
