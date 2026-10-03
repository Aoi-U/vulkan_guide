#pragma once

#include "app/editor/EditorPanel.h"
#include <imgui.h>
#include <ImGuizmo.h>

class GizmoPanel : public EditorPanel {
public:
  void on_draw(EditorContext& ctx) override;
  const char* get_name() const override { return "Gizmo Overlay"; }

private:
  ImGuizmo::OPERATION _currentOp{ ImGuizmo::TRANSLATE };
  ImGuizmo::MODE _currentMode{ ImGuizmo::LOCAL };
  bool _useSnap{ false };
  float _snapTranslation[3]{ 0.5f, 0.5f, 0.5f };
  float _snapRotation{ 45.0f };
  float _snapScale{ 0.1f };
};