#include "app/editor/ContentBrowserPanel.h"
#include <app/editor/GuiRenderer.h>
#include <graphics/Renderer.h>
#include <app/editor/UIWidgets.h>
#include <resources/AssetRegistry.h>
#include <resources/AssetImporter.h>
#include <resources/AssetSerializer.h>
#include <resources/DefaultResources.h>
#include <resources/ModelSpawner.h>
#include <resources/Mesh.h>
#include <scene/SceneSerializer.h>
#include <imgui.h>
#include <stb_image.h>
#include <fmt/core.h>
#include <algorithm>
#include <cctype>

void ContentBrowserPanel::init(const std::filesystem::path& assetsRoot)
{
	_rootDirectory = std::filesystem::exists(assetsRoot) ? std::filesystem::canonical(assetsRoot) : assetsRoot;
	_currentDirectory = _rootDirectory;
}

void ContentBrowserPanel::cleanup()
{
	_thumbnailTextures.clear();
}

void ContentBrowserPanel::on_imgui_render(EngineContext & ctx, EditorContext & editorCtx, GuiRenderer & guiRenderer)
{
	if (!ImGui::Begin("Content Browser")) {
		ImGui::End();
		return;
	}

  draw_top_bar(ctx);
  ImGui::Separator();

  // split panel: left directory tree (210px) | right asset card grid
  ImGui::BeginChild("DirectoryTreePane", ImVec2(210.0f, 0.0f), true);
  if (std::filesystem::exists(_rootDirectory)) {
    // root node
    ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (_currentDirectory == _rootDirectory) {
      rootFlags |= ImGuiTreeNodeFlags_Selected;
    }
    bool rootOpen = ImGui::TreeNodeEx("assets/", rootFlags);
    if (ImGui::IsItemClicked()) {
      _currentDirectory = _rootDirectory;
    }
    if (rootOpen) {
      draw_directory_tree(_rootDirectory);
      ImGui::TreePop();
    }
  }
  else {
    ImGui::TextDisabled("Assets directory not found:\n%s", _rootDirectory.string().c_str());
  }
  ImGui::EndChild();

  ImGui::SameLine();

  ImGui::BeginChild("AssetGridPane", ImVec2(0.0f, 0.0f), false);
  draw_asset_grid(ctx, editorCtx, guiRenderer);
  ImGui::EndChild();

  ImGui::End();

}

void ContentBrowserPanel::draw_directory_tree(const std::filesystem::path & directoryPath)
{
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(directoryPath, ec)) {
    if (!entry.is_directory()) continue;

    const auto& path = entry.path();
    std::string folderName = path.filename().string();
    if (!folderName.empty() && folderName[0] == '.') continue; // skip hidden cache folders

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (_currentDirectory == path) {
      flags |= ImGuiTreeNodeFlags_Selected;
    }

    // check if directory has subdirectories
    bool hasSubdirs = false;
    for (const auto& sub : std::filesystem::directory_iterator(path, ec)) {
      if (sub.is_directory()) {
        hasSubdirs = true;
        break;
      }
    }
    if (!hasSubdirs) {
      flags |= ImGuiTreeNodeFlags_Leaf;
    }

    bool opened = ImGui::TreeNodeEx(folderName.c_str(), flags);
    if (ImGui::IsItemClicked()) {
      _currentDirectory = path;
    }
    if (opened) {
      draw_directory_tree(path);
      ImGui::TreePop();
    }
  }
}

void ContentBrowserPanel::draw_top_bar(EngineContext & ctx)
{
  // breadcrumb bar and rescan buttons
  if (ImGui::Button("Rescan & Cook")) {
    AssetRegistry::get().scan_directory(_rootDirectory);
    AssetImporter::cook_missing_assets();
  }
  ImGui::SameLine();

  // new material button
  if (ImGui::Button("+ New Material")) {
    MaterialAssetData newMat{};
    newMat.uuid = UUID::generate();
    newMat.name = "NewMaterial_" + newMat.uuid.to_string().substr(0, 4);
    std::filesystem::path targetDir = std::filesystem::exists(_currentDirectory) ? _currentDirectory : _rootDirectory;
    std::filesystem::path matPath = targetDir / (newMat.name + ".mat");
    if (AssetSerializer::save_material_file(matPath, newMat)) {
      AssetRegistry::get().scan_directory(_rootDirectory);
    }
  }

  ImGui::SameLine();
  ImGui::TextDisabled("|");
  ImGui::SameLine();

  // breadcrumb trail from _rootDirectory to _currentDirectory
  if (ImGui::SmallButton("assets")) {
    _currentDirectory = _rootDirectory;
  }
  if (_currentDirectory != _rootDirectory) {
    std::error_code ec;
    std::filesystem::path rel = std::filesystem::relative(_currentDirectory, _rootDirectory, ec);
    if (!ec) {
      std::filesystem::path accum = _rootDirectory;
      for (const auto& part : rel) {
        if (part == ".") continue;
        accum /= part;
        ImGui::SameLine();
        ImGui::TextUnformatted("/");
        ImGui::SameLine();
        if (ImGui::SmallButton(part.string().c_str())) {
          _currentDirectory = accum;
        }
      }
    }
  }

  // search bar, filter pills, sub asset toggle
  ImGui::SetNextItemWidth(200.0f);
  ImGui::InputTextWithHint("##ContentSearch", "Search assets...", _searchBuffer.data(), sizeof(_searchBuffer));
  ImGui::SameLine();

  struct FilterPill { const char* label; AssetType type; };
  constexpr FilterPill pills[] = {
      { "All",       AssetType::Unknown },
      { "Models",    AssetType::Model },
      { "Meshes",    AssetType::Mesh },
      { "Materials", AssetType::Material },
      { "Textures",  AssetType::Texture },
      { "Prefabs",   AssetType::Prefab },
      { "Scenes",    AssetType::Scene },
      { "HDR",       AssetType::EnvironmentHDR }
  };

  for (const FilterPill& pill : pills) {
    bool active = (_selectedTypeFilter == pill.type);
    if (active) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.52f, 0.88f, 1.0f));
    }
    if (ImGui::SmallButton(pill.label)) {
      _selectedTypeFilter = pill.type;
    }
    if (active) {
      ImGui::PopStyleColor();
    }
    ImGui::SameLine();
  }

  ImGui::Checkbox("Show Sub-Assets (.gmesh/.gtex)", &_showSubAssets);
}

void ContentBrowserPanel::draw_asset_grid(EngineContext & ctx, EditorContext & editorCtx, GuiRenderer & guiRenderer)
{
  auto assets = AssetRegistry::get().get_all_assets_combined(_showSubAssets);

  std::string searchLower = _searchBuffer.data();
  std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
    });

  std::error_code ec;
  std::filesystem::path normCurrentDir = std::filesystem::weakly_canonical(_currentDirectory, ec);
  std::filesystem::path normRootDir = std::filesystem::weakly_canonical(_rootDirectory, ec);

  float padding = 12.0f;
  float cellSize = _thumbnailSize + padding;
  float panelWidth = ImGui::GetContentRegionAvail().x;
  int columnCount = std::max(1, static_cast<int>(panelWidth / cellSize));

  if (!ImGui::BeginTable("ContentBrowserGrid", columnCount)) {
    return;
  }

  for (const auto& meta : assets) {
    // filter by AssetType pill
    if (_selectedTypeFilter != AssetType::Unknown && meta.type != _selectedTypeFilter) {
      continue;
    }

    // filter by current directory 
    if (searchLower.empty() && _selectedTypeFilter == AssetType::Unknown && normCurrentDir != normRootDir) {
      std::filesystem::path assetParent = std::filesystem::weakly_canonical(meta.virtualPath.parent_path(), ec);
      if (assetParent != normCurrentDir) {
        continue;
      }
    }

    // filter by search string
    if (!searchLower.empty()) {
      std::string nameLower = meta.name;
      std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
        });
      if (nameLower.find(searchLower) == std::string::npos) {
        continue;
      }
    }

    ImGui::TableNextColumn();
    ImGui::PushID(static_cast<int>(meta.uuid.to_u64()));

    bool isSelected = (editorCtx.inspectorMode == InspectorMode::DiskAsset &&
      editorCtx.selectedAssetUUID == meta.uuid);

    if (isSelected) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.48f, 0.82f, 0.85f));
    }
    else {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.18f, 0.23f, 0.90f));
    }

    VkDescriptorSet thumbDesc = get_or_create_thumbnail(meta, ctx, guiRenderer);
    if (thumbDesc != VK_NULL_HANDLE) {
      if (ImGui::ImageButton("##ThumbBtn", (ImTextureID)thumbDesc, ImVec2(_thumbnailSize, _thumbnailSize))) {
        editorCtx.select_disk_asset(meta.uuid);
      }
    }
    else {
      std::string fallbackLabel = fmt::format("[{}]\n{}", AssetUtils::asset_type_to_string(meta.type), meta.name);
      if (ImGui::Button(fallbackLabel.c_str(), ImVec2(_thumbnailSize, _thumbnailSize))) {
        editorCtx.select_disk_asset(meta.uuid);
      }
    }
    ImGui::PopStyleColor();

    // drag and drop source carrying UUID
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
      ImGui::SetDragDropPayload(IMGUI_PAYLOAD_TYPE_ASSET_UUID, &meta.uuid, sizeof(UUID));
      ImGui::Text("%s (%s)", meta.name.c_str(), AssetUtils::asset_type_to_string(meta.type).data());
      UIWidgets::draw_residency_badge(meta.state, meta.gpuSizeBytes);
      ImGui::EndDragDropSource();
    }

    // double click action: spawn Prefab/Model or Load Scene/HDR
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      if ((meta.type == AssetType::Model || meta.type == AssetType::Prefab) && ctx.activeRegistry && ctx.resourceManager) {
        entt::entity root = spawn_prefab(*ctx.activeRegistry, *ctx.resourceManager, meta.virtualPath);
        if (root != entt::null) {
          editorCtx.select_entity(root);
          if (ctx.renderer) ctx.renderer->mark_scene_dirty();
        }
      }
      else if (meta.type == AssetType::EnvironmentHDR && ctx.renderer) {
        ctx.renderer->load_environment(meta.virtualPath);
      }
    }

    // right click context menu 
    if (ImGui::BeginPopupContextItem("AssetCardContextMenu")) {
      ImGui::TextDisabled("%s (%s)", meta.name.c_str(), AssetUtils::asset_type_to_string(meta.type).data());
      ImGui::Separator();

      if (ImGui::MenuItem("Inspect Asset")) {
        editorCtx.select_disk_asset(meta.uuid);
      }

      if (meta.type == AssetType::Texture || meta.type == AssetType::Mesh) {
        if (ImGui::MenuItem("Load / Stream into GPU", nullptr, false, meta.state == ResourceState::Unloaded)) {
          if (ctx.resourceManager) {
            ctx.resourceManager->request_async_load(meta.uuid);
          }
        }
        if (ImGui::MenuItem("Force Unload from GPU", nullptr, false, meta.state == ResourceState::Loaded)) {
          if (ctx.resourceManager) {
            if (meta.type == AssetType::Texture) ctx.resourceManager->force_unload<Texture>(meta.uuid);
            if (meta.type == AssetType::Mesh)    ctx.resourceManager->force_unload<Mesh>(meta.uuid);
            if (ctx.renderer)                    ctx.renderer->mark_scene_dirty();
          }
        }
      }

      if (meta.type == AssetType::Texture && !meta.is_sub_asset()) {
        if (ImGui::MenuItem("Reimport / Recook .gtex")) {
          AssetImporter::cook_texture(meta);
        }
      }
      else if (meta.type == AssetType::Model) {
        if (ImGui::MenuItem("Reimport / Disaggregate glTF")) {
          AssetImporter::import_model(meta);
        }
      }

      if ((meta.type == AssetType::Model || meta.type == AssetType::Prefab) && ctx.activeRegistry && ctx.resourceManager) {
        if (ImGui::MenuItem("Spawn in Scene")) {
          entt::entity spawned = spawn_prefab(*ctx.activeRegistry, *ctx.resourceManager, meta.virtualPath);
          editorCtx.select_entity(spawned);
          if (ctx.renderer) ctx.renderer->mark_scene_dirty();
        }
      }

      ImGui::EndPopup();
    }

    // residency badg, truncated label underneath card
    UIWidgets::draw_residency_badge(meta.state, meta.gpuSizeBytes);
    ImGui::TextWrapped("%s", meta.name.c_str());

    ImGui::PopID();
  }

  ImGui::EndTable();
}

VkDescriptorSet ContentBrowserPanel::get_or_create_thumbnail(const AssetMetadata & meta, EngineContext & ctx, GuiRenderer & guiRenderer)
{
  if (!ctx.resourceManager) return VK_NULL_HANDLE;

  auto it = _thumbnailTextures.find(meta.uuid);
  if (it != _thumbnailTextures.end() && it->second.is_valid()) {
    return guiRenderer.get_image_descriptor(it->second->get_image_view(), it->second->get_sampler());
  }

  // generate 16x16 color coded icon per AssetType so unloaded disk assets take < 1 KB in the thumbnail cache
  glm::vec4 baseColor(0.25f, 0.28f, 0.35f, 1.0f);
  switch (meta.type) {
  case AssetType::Texture:        baseColor = glm::vec4(0.20f, 0.65f, 0.55f, 1.0f); break;
  case AssetType::Material:       baseColor = glm::vec4(0.85f, 0.45f, 0.20f, 1.0f); break;
  case AssetType::Model:
  case AssetType::Prefab:         baseColor = glm::vec4(0.35f, 0.55f, 0.90f, 1.0f); break;
  case AssetType::Mesh:           baseColor = glm::vec4(0.55f, 0.40f, 0.85f, 1.0f); break;
  case AssetType::Scene:          baseColor = glm::vec4(0.85f, 0.70f, 0.20f, 1.0f); break;
  case AssetType::EnvironmentHDR: baseColor = glm::vec4(0.75f, 0.30f, 0.60f, 1.0f); break;
  default: break;
  }

  uint32_t packedMain = glm::packUnorm4x8(baseColor);
  uint32_t packedBorder = glm::packUnorm4x8(baseColor * 0.6f);
  std::array<uint32_t, 16 * 16> iconPixels{};
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      bool isBorder = (x == 0 || x == 15 || y == 0 || y == 15);
      iconPixels[y * 16 + x] = isBorder ? packedBorder : packedMain;
    }
  }

  TextureDescription desc{
    .extent = { 16, 16, 1 },
    .format = VK_FORMAT_R8G8B8A8_UNORM,
    .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    .mipLevels = 1,
    .magFilter = VK_FILTER_NEAREST,
    .minFilter = VK_FILTER_NEAREST,
    .deferBindlessRegistration = true // UI thumbnails dont consume bindless material slots
  };

  std::string thumbId = "thumb_" + meta.uuid.to_string();
  auto handle = ctx.resourceManager->load<Texture>(
    thumbId,
    desc,
    std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(iconPixels.data()), sizeof(iconPixels))
  );

  if (handle.is_valid()) {
    _thumbnailTextures[meta.uuid] = handle;
    return guiRenderer.get_image_descriptor(handle->get_image_view(), handle->get_sampler());
  }
  return VK_NULL_HANDLE;
}
