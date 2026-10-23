#include "AssetRegistry.h"

#include <resources/AssetSerializer.h>
#include <fmt/core.h>
#include <algorithm>
#include <chrono>
#include <cctype>

std::string AssetRegistry::normalize_path_key(const std::filesystem::path& path)
{
	std::string key = path.lexically_normal().generic_string();
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { 
    return static_cast<char>(std::tolower(c)); 
    });
	return key;
}

uint64_t AssetRegistry::get_file_timestamp(const std::filesystem::path& path)
{
	std::error_code ec;
	auto ftime = std::filesystem::last_write_time(path, ec);
	if (ec)
		return 0;
	return static_cast<uint64_t>(ftime.time_since_epoch().count());
}

std::filesystem::path AssetRegistry::get_default_cooked_path(const UUID& uuid, AssetType type)
{
	switch (type) {
	case AssetType::Texture:
	case AssetType::EnvironmentHDR:
		return std::filesystem::path(".cache/textures") / (uuid.to_string() + ".gtex");
	case AssetType::Mesh:
	case AssetType::Model:
		return std::filesystem::path(".cache/meshes") / (uuid.to_string() + ".gmesh");
	default:
		return {};
	}
}

void AssetRegistry::scan_directory(const std::filesystem::path& rootPath)
{
	auto startTime = std::chrono::high_resolution_clock::now();

	std::unique_lock lock(_mutex);
	_assetsByUUID.clear();
	_uuidByNormalizedPath.clear();
	_uuidById.clear();
	_assetListCache.clear();

	if (!std::filesystem::exists(rootPath)) {
		fmt::println("AssetRegistry: directory does not exist: {}", rootPath.string());
			return;
	}

	for (const auto& entry : std::filesystem::recursive_directory_iterator(rootPath)) {
		if (!entry.is_regular_file())
			continue;

		const std::filesystem::path& filePath = entry.path();
		std::string ext = filePath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { 
      return static_cast<char>(std::tolower(c)); 
      });

		if (ext == ".meta" || ext == ".spv")
			continue;

		AssetType deducedType = AssetUtils::asset_type_from_extension(filePath);
		if (deducedType == AssetType::Unknown)
			continue;

		std::filesystem::path metaPath = filePath;
		metaPath += ".meta";

		AssetMetadata meta{};
    bool loadedFromMeta = AssetSerializer::load_meta_file(metaPath, meta);
    uint64_t currentWriteTime = get_file_timestamp(filePath);
    std::error_code ec;
    uint64_t fileSize = std::filesystem::file_size(filePath, ec);

    // determine canonical asset id, match existing shader filename vs stem convention
    std::string canonicalId = (deducedType == AssetType::Shader) ? filePath.filename().string() : filePath.stem().string();

    // if this is a .mat or .prefab file, prefer the authoritative UUID stored inside its YAML
    UUID embeddedUuid = UUID::null();
    UUID embeddedParentUuid = UUID::null();
    if (deducedType == AssetType::Material) {
      MaterialAssetData matData{};
      if (AssetSerializer::load_material_file(filePath, matData) && matData.uuid.is_valid()) {
        embeddedUuid = matData.uuid;
      }
    }
    else if (deducedType == AssetType::Prefab) {
      PrefabAssetData prefabData{};
      if (AssetSerializer::load_prefab_file(filePath, prefabData) && prefabData.uuid.is_valid()) {
        embeddedUuid = prefabData.uuid;
        embeddedParentUuid = prefabData.sourceModelUUID;
      }
    }

    bool needsMetaSave = false;
    if (!loadedFromMeta || !meta.uuid.is_valid()) {
      meta.uuid = embeddedUuid.is_valid() ? embeddedUuid : UUID::generate();
      meta.type = deducedType;
      meta.name = canonicalId;
      meta.cookedPath = get_default_cooked_path(meta.uuid, meta.type);

      if (meta.type == AssetType::Texture && meta.textureSettings.virtualTexture)
				meta.cookedPath = std::filesystem::path(".cache/textures") / (meta.uuid.to_string() + ".vtex");

      // default import settings
      if (meta.type == AssetType::Texture) {
        std::string lowerName = canonicalId;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char c) { 
          return static_cast<char>(std::tolower(c)); 
          });
        meta.textureSettings.sRGB = (lowerName.find("albedo") != std::string::npos ||
          lowerName.find("basecolor") != std::string::npos ||
          lowerName.find("diffuse") != std::string::npos ||
          lowerName.find("emissive") != std::string::npos);
      }
      needsMetaSave = true;
    }
    else if (embeddedUuid.is_valid() && meta.uuid != embeddedUuid) {
      meta.uuid = embeddedUuid;
      needsMetaSave = true;
    }
    else if (meta.lastWriteTime != currentWriteTime) {
      needsMetaSave = true;
    }

    if (embeddedParentUuid.is_valid() && !meta.parentAssetUUID.is_valid()) {
      meta.parentAssetUUID = embeddedParentUuid;
    }
    if (auto existingIt = _assetsByUUID.find(meta.uuid); existingIt != _assetsByUUID.end()) {
      if (existingIt->second.parentAssetUUID.is_valid() && !meta.parentAssetUUID.is_valid()) {
        meta.parentAssetUUID = existingIt->second.parentAssetUUID;
      }
    }

    meta.type = deducedType;
    meta.name = canonicalId;
    meta.id = canonicalId;
    meta.virtualPath = filePath;
    meta.path = filePath;
    meta.diskSizeBytes = ec ? 0 : fileSize;
    meta.gpuSizeBytes = 0;
    meta.lastWriteTime = currentWriteTime;
    meta.state = ResourceState::Unloaded;

    if (meta.cookedPath.empty()) {
      meta.cookedPath = get_default_cooked_path(meta.uuid, meta.type);
    }

    if (needsMetaSave) {
      AssetSerializer::save_meta_file(metaPath, meta);
    }

    std::string normPath = normalize_path_key(filePath);
    _uuidByNormalizedPath[normPath] = meta.uuid;

    // do not let a generated .prefab overwrite a primary Model with the same stem in _uuidById
    bool shouldRegisterId = true;
    if (meta.type == AssetType::Prefab) {
      if (auto idIt = _uuidById.find(meta.id); idIt != _uuidById.end()) {
        if (auto existingAssetIt = _assetsByUUID.find(idIt->second); existingAssetIt != _assetsByUUID.end() && existingAssetIt->second.type == AssetType::Model) {
          shouldRegisterId = false;
        }
      }
    }
    if (shouldRegisterId) {
      _uuidById[meta.id] = meta.uuid;
    }

    // register any subassets stored inside containers meta file
    for (const auto& sub : meta.subAssets) {
      if (!sub.uuid.is_valid()) continue;
      AssetMetadata subMeta{};
      subMeta.uuid = sub.uuid;
      subMeta.type = sub.type;
      subMeta.name = sub.name;
      subMeta.id = sub.name;
      subMeta.virtualPath = filePath;
      subMeta.path = filePath;
      subMeta.parentAssetUUID = meta.uuid;
      subMeta.cookedPath = get_default_cooked_path(sub.uuid, sub.type);
      subMeta.state = ResourceState::Unloaded;

      if (sub.type == AssetType::Material) {
        std::filesystem::path matPath = filePath.parent_path() / "materials" / (sub.name + ".mat");
        subMeta.virtualPath = matPath;
        subMeta.path = matPath;
        subMeta.cookedPath = matPath;
        _uuidByNormalizedPath[normalize_path_key(matPath)] = subMeta.uuid;
      }
      else if (sub.type == AssetType::Prefab) {
        std::filesystem::path prefabPath = filePath;
        prefabPath.replace_extension(".prefab");
        subMeta.virtualPath = prefabPath;
        subMeta.path = prefabPath;
        subMeta.cookedPath = prefabPath;
        _uuidByNormalizedPath[normalize_path_key(prefabPath)] = subMeta.uuid;
      }
      else {
        subMeta.virtualPath = filePath;
        subMeta.path = filePath;
        subMeta.cookedPath = get_default_cooked_path(sub.uuid, sub.type);
      }

      _assetsByUUID[subMeta.uuid] = subMeta;
      if (sub.type != AssetType::Prefab) {
        _uuidById[subMeta.id] = subMeta.uuid;
      }
    }

    _assetsByUUID[meta.uuid] = meta;
    _assetListCache.push_back(std::move(meta));
	}

  auto endTime = std::chrono::high_resolution_clock::now();
  _lastScanDurationMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
}

const AssetMetadata* AssetRegistry::get_metadata(const UUID & uuid) const
{
  std::shared_lock lock(_mutex);
  auto it = _assetsByUUID.find(uuid);
  return (it != _assetsByUUID.end()) ? &it->second : nullptr;
}

AssetMetadata* AssetRegistry::get_metadata_mutable(const UUID& uuid)
{
  std::unique_lock lock(_mutex);
  auto it = _assetsByUUID.find(uuid);
  return (it != _assetsByUUID.end()) ? &it->second : nullptr;
}

bool AssetRegistry::contains(const UUID& uuid) const
{
  std::shared_lock lock(_mutex);
  return _assetsByUUID.contains(uuid);
}

UUID AssetRegistry::find_uuid_by_path(const std::filesystem::path& path) const
{
  std::shared_lock lock(_mutex);
  auto it = _uuidByNormalizedPath.find(normalize_path_key(path));
  return (it != _uuidByNormalizedPath.end()) ? it->second : UUID::null();
}

UUID AssetRegistry::find_uuid_by_id(std::string_view id) const
{
  std::shared_lock lock(_mutex);
  auto it = _uuidById.find(std::string(id));
  return (it != _uuidById.end()) ? it->second : UUID::null();
}

const AssetMetadata* AssetRegistry::find_asset_by_path(const std::filesystem::path& path) const
{
  UUID uuid = find_uuid_by_path(path);
  return uuid.is_valid() ? get_metadata(uuid) : nullptr;
}

const AssetMetadata* AssetRegistry::find_asset_by_id(const std::string& id) const
{
  UUID uuid = find_uuid_by_id(id);
  if (uuid.is_valid())
    return get_metadata(uuid);

  fmt::println("AssetRegistery: no asset found with ID {}", id);
  return nullptr;
}

UUID AssetRegistry::resolve_or_derive_uuid(std::string_view identifier) const
{
  if (identifier.empty())
    return UUID::null();

  // check if identifier already a registed asset id/stem
  UUID found = find_uuid_by_id(identifier);
  if (found.is_valid())
    return found;

  // check if identifier is file path
	found = find_uuid_by_path(std::filesystem::path(identifier));
  if (found.is_valid())
    return found;

  // fallback, hash runtime resource names
  return UUID::from_name(identifier);
}

UUID AssetRegistry::register_sub_asset(const UUID& parentUUID, std::string_view subName, AssetType subType, const std::filesystem::path& cookedPath)
{
  UUID subUUID = UUID::derive(parentUUID, subName);

  std::unique_lock lock(_mutex);
  auto parentIt = _assetsByUUID.find(parentUUID);

	// register sub-asset metadata
  AssetMetadata subMeta{};
  subMeta.uuid = subUUID;
  subMeta.type = subType;
  subMeta.name = std::string(subName);
  subMeta.id = std::string(subName);
  subMeta.parentAssetUUID = parentUUID;
  subMeta.cookedPath = cookedPath.empty() ? get_default_cooked_path(subUUID, subType) : cookedPath;
  subMeta.state = ResourceState::Unloaded;

  if (parentIt != _assetsByUUID.end()) {
    if (subType == AssetType::Material) {
      std::filesystem::path matPath = parentIt->second.virtualPath.parent_path() / "materials" / (std::string(subName) + ".mat");
      subMeta.virtualPath = matPath;
      subMeta.path = matPath;
      if (cookedPath.empty()) subMeta.cookedPath = matPath;
      _uuidByNormalizedPath[normalize_path_key(matPath)] = subUUID;
    }
    else if (subType == AssetType::Prefab) {
      std::filesystem::path prefabPath = parentIt->second.virtualPath;
      prefabPath.replace_extension(".prefab");
      subMeta.virtualPath = prefabPath;
      subMeta.path = prefabPath;
      if (cookedPath.empty()) subMeta.cookedPath = prefabPath;
      _uuidByNormalizedPath[normalize_path_key(prefabPath)] = subUUID;
    }
    else {
      subMeta.virtualPath = parentIt->second.virtualPath;
      subMeta.path = parentIt->second.path;
    }

    std::vector<SubAssetEntry>& subs = parentIt->second.subAssets;
    auto existingSub = std::find_if(subs.begin(), subs.end(), [&](const SubAssetEntry& e) {
      return e.uuid == subUUID;
      });
    if (existingSub == subs.end()) {
      subs.emplace_back(std::string(subName), subUUID, subType);
      std::filesystem::path metaPath = parentIt->second.virtualPath;
      metaPath += ".meta";
      AssetSerializer::save_meta_file(metaPath, parentIt->second);

      for (AssetMetadata& cached : _assetListCache) {
        if (cached.uuid == parentUUID) {
          cached.subAssets = parentIt->second.subAssets;
          break;
        }
      }
    }
  }

  _assetsByUUID[subUUID] = std::move(subMeta);
  if (subType != AssetType::Prefab) {
    _uuidById[std::string(subName)] = subUUID;
  }
  return subUUID;
}

void AssetRegistry::set_resource_state(const UUID& uuid, ResourceState state)
{
  std::unique_lock lock(_mutex);
  if (auto it = _assetsByUUID.find(uuid); it != _assetsByUUID.end()) {
    it->second.state = state;

		for (auto& cached : _assetListCache) { // update cached to new state
      if (cached.uuid == uuid) {
        cached.state = state;
        break;
      }
    }
  }
}

void AssetRegistry::set_gpu_size(const UUID & uuid, uint64_t gpuBytes)
{
  std::unique_lock lock(_mutex);
  if (auto it = _assetsByUUID.find(uuid); it != _assetsByUUID.end()) {
		it->second.gpuSizeBytes = gpuBytes;
		for (AssetMetadata& cached : _assetListCache) { // update cached to new size
			if (cached.uuid == uuid) {
				cached.gpuSizeBytes = gpuBytes;
				break;
			}
		}
  }
}

ResourceState AssetRegistry::get_resource_state(const UUID & uuid) const
{
  std::shared_lock lock(_mutex);
  auto it = _assetsByUUID.find(uuid);
  return (it != _assetsByUUID.end()) ? it->second.state : ResourceState::Unloaded;
}

std::vector<AssetMetadata> AssetRegistry::get_assets_by_type(AssetType type, bool includeSubAssets) const
{
  std::shared_lock lock(_mutex);
  std::vector<AssetMetadata> filtered;
  
  if (!includeSubAssets) {
    for (const AssetMetadata& asset : _assetListCache) {
      if (asset.type == type) {
        filtered.push_back(asset);
      }
    }
  }
  else {
    for (const auto& [_, asset] : _assetsByUUID) {
      if (asset.type == type) {
        filtered.push_back(asset);
      }
    }
    std::sort(filtered.begin(), filtered.end(), [](const AssetMetadata& a, const AssetMetadata& b) {
      return a.name < b.name;
      });
  }
  
  return filtered;
}

std::vector<AssetMetadata> AssetRegistry::get_all_assets_combined(bool includeSubAssets) const
{
  std::shared_lock lock(_mutex);
  if (!includeSubAssets) {
    return _assetListCache;
  }

  size_t assetCount = _assetsByUUID.size();

  std::vector<const AssetMetadata*> pointerVector;
  pointerVector.reserve(assetCount);
  for (const auto& [_, meta] : _assetsByUUID) {
    pointerVector.push_back(&meta);
  }

  std::sort(pointerVector.begin(), pointerVector.end(), [](const AssetMetadata* a, const AssetMetadata* b) {
    return a->name < b->name;
    });

  std::vector<AssetMetadata> result;
  result.reserve(assetCount);
  for (const auto* ptr : pointerVector) {
    result.push_back(*ptr);
  }

  return result;
}

void AssetRegistry::print_manifest() const
{
  std::shared_lock lock(_mutex);
  uint64_t totalDiskBytes = 0;
  uint64_t totalGpuBytes = 0;

  fmt::println("AssetRegistery manifest ({} disk assets scanned in {:.2f} ms)", _assetListCache.size(), _lastScanDurationMs);
  for (const AssetMetadata& meta : _assetListCache) {
    totalDiskBytes += meta.diskSizeBytes;
    totalGpuBytes += meta.gpuSizeBytes;
    fmt::println(" [{}] {:<14} | {:<28} | Disk: {:>7.1f} KB | GPU: {:>4} B | {}",
      meta.uuid,
      AssetUtils::asset_type_to_string(meta.type),
      meta.name,
      static_cast<double>(meta.diskSizeBytes) / 1024.0,
      meta.gpuSizeBytes,
      AssetUtils::resource_state_to_string(meta.state));
  }
	fmt::println("Total disk size: {:.2f} MB, Total GPU size: {:.2f} MB", static_cast<double>(totalDiskBytes) / (1024.0 * 1024.0), static_cast<double>(totalGpuBytes) / (1024.0 * 1024.0));
}
