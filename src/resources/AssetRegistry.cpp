#include "AssetRegistry.h"
#include <algorithm>
#include <fmt/core.h>

void AssetRegistry::scan_directory(const std::filesystem::path& rootPath)
{
	_assets.clear();

	if (!std::filesystem::exists(rootPath)) {
		fmt::println("AssetRegistry: Directory does not exist: {}", rootPath.string());
		return;
	}

	// scan directory using std filesystem
	for (const auto& entry : std::filesystem::recursive_directory_iterator(rootPath)) {
		if (entry.is_regular_file()) {
			AssetRecord record;
			record.path = entry.path();
			record.id = entry.path().stem().string();
			std::string ext = record.path.extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

			if (ext == ".glb" || ext == ".gltf") // model
				record.type = AssetType::Model;

			else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") // texture
				record.type = AssetType::Texture;
			
			else if (ext == ".hdr") // environment map
				record.type = AssetType::EnvironmentHDR;
			
			else if (ext == ".vert" || ext == ".frag" || ext == ".comp") { // shader 
				record.type = AssetType::Shader;
				record.id = entry.path().filename().string();
			}
			else // unsupported
				record.type = AssetType::Unknown;
			
			if (record.type != AssetType::Unknown) {
				_assets.push_back(record);
			}
		}
	}
}

std::vector<AssetRecord> AssetRegistry::get_assets_by_type(AssetType type) const
{
	std::vector<AssetRecord> filtered;
	for (const auto& asset : _assets) {
		if (asset.type == type)
			filtered.push_back(asset);
	}
	return filtered;
}

const AssetRecord* AssetRegistry::find_asset_by_path(const std::filesystem::path& path) const
{
	for (const auto& asset : _assets) {
		if (asset.path == path)
			return &asset;
	}

	//fmt::println("AssetRegistry: No asset found at path: {}", path.string());
	return nullptr;
}

const AssetRecord* AssetRegistry::find_asset_by_id(const std::string& id) const
{
	for (const auto& asset : _assets) {
		if (asset.id == id)
			return &asset;
	}

	fmt::println("AssetRegistry: No asset found with ID: {}", id);
	return nullptr;
}
