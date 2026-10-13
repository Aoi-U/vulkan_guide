#pragma once

#include <string>
#include <vector>
#include <filesystem>

enum class AssetType
{
	Texture,
	Model,
	Shader,
	EnvironmentHDR,
	Unknown
};

struct AssetRecord
{
	std::string id;
	std::filesystem::path path;
	AssetType type;
};

class AssetRegistry
{
public:
	static AssetRegistry& get() 
	{
		static AssetRegistry instance;
		return instance;
	}

	void scan_directory(const std::filesystem::path& rootPath);
	
	std::vector<AssetRecord> get_assets_by_type(AssetType type) const;
	const AssetRecord* find_asset_by_path(const std::filesystem::path& path) const;
	const AssetRecord* find_asset_by_id(const std::string& id) const;
	const std::vector<AssetRecord>& get_all_assets() const { return _assets; }

private:
	std::vector<AssetRecord> _assets;
};