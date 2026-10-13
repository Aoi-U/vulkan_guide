#pragma once

#include "app/editor/EditorPanel.h"
#include <string>

class RenderTargetViewPanel : public EditorPanel {
public:
  void on_draw(EditorContext& ctx) override;
  const char* get_name() const override { return "Render Targets"; }

private:
  std::string _selectedTarget{ "SceneColor" };
  int _selectedCubeFace{ 0 };
  float _zoomScale{ 1.0f };
  bool _fitToWindow{ true };
};