#include "app/editor/ContentBrowserPanel.h"

#include <resources/AssetRegistry.h>
#include <resources/ModelSpawner.h>
#include <resources/Model.h>
#include <graphics/Renderer.h>
#include <core/Components.h>
#include <imgui.h>

ContentBrowserPanel::ContentBrowserPanel()
{
	_baseDirectory = "../../assets";
	_currentDirectory = _baseDirectory;
}

void ContentBrowserPanel::on_draw(EditorContext & ctx)
{
	if (!is_open)
		return;

	ImGui::Begin(get_name(), &is_open);

	if (_currentDirectory != _baseDirectory) {
		if (ImGui::Button("<-")) {
			_currentDirectory = _currentDirectory.parent_path();
		}
		ImGui::SameLine();
	}
	ImGui::Text("%s", _currentDirectory.string().c_str());
	ImGui::Separator();

	static float padding = 16.0f;
	static float thumbnailSize = 90.0f;
	float cellSize = thumbnailSize + padding;
	float panelWidth = ImGui::GetContentRegionAvail().x;
	int columnCount = std::max(1, (int)(panelWidth / cellSize));

	ImGui::Columns(columnCount, 0, false);

  // iterate through current directory to display folders and files
  for (const auto& entry : std::filesystem::directory_iterator(_currentDirectory)) {
    const auto& path = entry.path();
    std::string filenameString = path.filename().string();

    ImGui::PushID(filenameString.c_str());

    // display folder
    if (entry.is_directory()) {
      if (ImGui::Button(filenameString.c_str(), { thumbnailSize, thumbnailSize })) {
        _currentDirectory /= path.filename();
      }
      ImGui::TextWrapped("%s", filenameString.c_str());
    }
    else { // display file
      auto assetRec = AssetRegistry::get().find_asset_by_path(path);
      if (assetRec) {
        bool isLoaded = false, isPersistent = false;
        if (assetRec->type == AssetType::Model) {
          isLoaded = ctx.resourceManager.has_resource<Model>(assetRec->id);
        }

        ImVec4 btnColor = isLoaded ? ImVec4(0.2f, 0.4f, 0.8f, 1.0f) : ImVec4(0.2f, 0.2f, 0.2f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, btnColor);
        ImGui::Button(assetRec->id.c_str(), { thumbnailSize, thumbnailSize });
        ImGui::PopStyleColor();

        // drag and drop source
        if ((assetRec->type == AssetType::Model || assetRec->type == AssetType::Prefab) && ImGui::BeginDragDropSource()) {
          std::string pathStr = assetRec->path.string();
          ImGui::SetDragDropPayload("ASSET_MODEL_ID", pathStr.c_str(), pathStr.size() + 1);
          ImGui::Text("Spawning %s", assetRec->id.c_str());
          ImGui::EndDragDropSource();
        }
        else if (assetRec->type == AssetType::Texture && ImGui::BeginDragDropSource()) {
          ImGui::SetDragDropPayload("ASSET_TEXTURE_ID", assetRec->id.c_str(), assetRec->id.size() + 1);
          ImGui::Text("Texture: %s", assetRec->id.c_str());
          ImGui::EndDragDropSource();
        }

        // context menu
        if (ImGui::BeginPopupContextItem()) {
          if (assetRec->type == AssetType::Model || assetRec->type == AssetType::Prefab) {
            if (ImGui::MenuItem("Spawn Entity")) {
              ctx.selectedEntity = spawn_prefab(ctx.registry, ctx.resourceManager, assetRec->path);
              ctx.renderer.mark_scene_dirty();
            }
          }
          ImGui::EndPopup();
        }

        ImGui::TextWrapped("%s", filenameString.c_str());
        if (isLoaded) 
          ImGui::TextColored({ 0.4f, 0.8f, 1.0f, 1.0f }, "[Loaded]");
        else 
          ImGui::TextColored({ 0.6f, 0.6f, 0.6f, 1.0f }, "[On Disk]");
      }
    }
    ImGui::NextColumn();
    ImGui::PopID();
  }
  ImGui::Columns(1);
  ImGui::End();
}
