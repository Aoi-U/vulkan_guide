#pragma once

#include <cstdint>
#include <filesystem>
#include <resources/AssetMetadata.h>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// singleton registry for all assets in the project
class AssetRegistry
{
public:
	static AssetRegistry& get() 
	{
		static AssetRegistry instance;
		return instance;
	}

	void scan_directory(const std::filesystem::path& rootPath);

	// uuid queries
	const AssetMetadata* get_metadata(const UUID& uuid) const;
	AssetMetadata* get_metadata_mutable(const UUID& uuid);
	bool contains(const UUID& uuid) const;

	// path/name queries
	UUID find_uuid_by_path(const std::filesystem::path& path) const;
	UUID find_uuid_by_id(std::string_view id) const;
	const AssetMetadata* find_asset_by_path(const std::filesystem::path& path) const;
	const AssetMetadata* find_asset_by_id(const std::string& id) const;

	// resolve existing disk asset uuid by path/name or create uuid for runtime resources
	UUID resolve_or_derive_uuid(std::string_view identifier) const;

	// register sub-asset under parent asset
	UUID register_sub_asset(const UUID& parentUUID, std::string_view subName, AssetType subType, const std::filesystem::path& cookedPath = "");

	// runtime state tracking
	void set_resource_state(const UUID& uuid, ResourceState state);
	void set_gpu_size(const UUID& uuid, uint64_t gpuBytes);
	ResourceState get_resource_state(const UUID& uuid) const;

	void set_texture_virtual_state(const UUID& texUuid, bool isVirtual, bool saveMeta = true);

	std::vector<AssetMetadata> get_assets_by_type(AssetType type, bool includeSubAssets = true) const;
	std::vector<AssetMetadata> get_all_assets_combined(bool includeSubAssets = false) const;
	const std::vector<AssetMetadata>& get_all_assets() const { return _assetListCache; }

	void print_manifest() const;

private:
	static std::string normalize_path_key(const std::filesystem::path& path);
	static uint64_t get_file_timestamp(const std::filesystem::path& path);
	static std::filesystem::path get_default_cooked_path(const UUID& uuid, AssetType type);

	mutable std::shared_mutex _mutex;
	
	std::unordered_map<UUID, AssetMetadata> _assetsByUUID;
	std::unordered_map<std::string, UUID> _uuidByNormalizedPath;
	std::unordered_map<std::string, UUID> _uuidById;
	std::vector<AssetMetadata> _assetListCache;
	double _lastScanDurationMs{ 0.0f };
};