#include "app/editor/EnvironmentPanel.h"

#include <core/Events.h>
#include <filesystem>
#include <imgui.h>

void EnvironmentPanel::on_draw(EditorContext& ctx)
{
  if (!is_open) return;
  ImGui::Begin(get_name(), &is_open);

  auto& dispatcher = ctx.registry.ctx().get<entt::dispatcher>();
  ImGui::Text("Active Source: %s", _currentEnvLabel.c_str());
  ImGui::Separator();

  ImGui::InputText("HDR File", _hdrPathBuffer.data(), _hdrPathBuffer.size());
  const std::filesystem::path targetPath(_hdrPathBuffer.data());
  const bool exists = std::filesystem::exists(targetPath);

  if (!exists && _hdrPathBuffer[0] != '\0') {
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Warning: File does not exist!");
  }

  if (ImGui::Button("Load HDR")) {
    if (exists) {
      dispatcher.trigger<Events::LoadEnvironmentEvent>({ targetPath });
      _currentEnvLabel = targetPath.filename().string();
      _statusMsg = "Loaded: " + _currentEnvLabel;
      _isError = false;
    }
    else {
      _statusMsg = "Failed: File path not found.";
      _isError = true;
    }
  }

  ImGui::SameLine();
  if (ImGui::Button("Reset to Procedural")) {
    dispatcher.trigger<Events::ResetEnvironmentEvent>({});
    _currentEnvLabel = "Procedural Sky";
    _statusMsg = "Reset to procedural sky.";
    _isError = false;
  }

  if (!_statusMsg.empty()) {
    ImGui::Separator();
    ImVec4 col = _isError ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f) : ImVec4(0.3f, 0.8f, 0.3f, 1.0f);
    ImGui::TextColored(col, "%s", _statusMsg.c_str());
  }

  ImGui::End();
}
