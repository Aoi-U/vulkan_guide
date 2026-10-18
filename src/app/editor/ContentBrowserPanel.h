#pragma once

#include <core/layer/EngineContext.h>
#include <app/editor/EditorContext.h>
#include <resources/AssetMetadata.h>
#include <resources/Texture.h>
#include <filesystem>
#include <string>
#include <unordered_map>

class GuiRenderer;

class ContentBrowserPanel 
{
public:
	void init(const std::filesystem::path& assetsRoot = "../../assets");
	void cleanup();

	void on_imgui_render(EngineContext& ctx, EditorContext& editorCtx, GuiRenderer& guiRenderer);

private:
	void draw_directory_tree(const std::filesystem::path& directoryPath);
	void draw_top_bar(EngineContext& ctx);
	void draw_asset_grid(EngineContext& ctx, EditorContext& editorCtx, GuiRenderer& guiRenderer);

	VkDescriptorSet get_or_create_thumbnail(const AssetMetadata& meta, EngineContext& ctx, GuiRenderer& guiRenderer);

	std::filesystem::path _rootDirectory{ "../../assets" };
	std::filesystem::path _currentDirectory{ "../../assets" };

	std::array<char, 128> _searchBuffer{};
	AssetType _selectedTypeFilter{ AssetType::Unknown };
	bool _showSubAssets{ false };
	float _thumbnailSize{ 88.0f };

	std::unordered_map<UUID, ResourceHandle<Texture>> _thumbnailTextures;
};