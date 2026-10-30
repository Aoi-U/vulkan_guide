#pragma once

#include <core/UUID.h>
#include <vulkan/vulkan.h>
#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>

// asset related types and metadata

enum class AssetType : uint8_t
{
	Texture,
	Model,
	Mesh,
	Material,
	Shader,
	EnvironmentHDR,
	Scene,
	Prefab,
	Unknown
};

enum class ResourceState : uint8_t
{
	Unloaded, // on disk
	Queued, // request by scene/editor
	Loading, // worker thread reading/uploading
	Loaded, // loaded in resourcemanager
	Failed // missing file or corrupted
};

struct TextureImportSettings
{
	bool sRGB{ false };
	bool generateMips{ true };
	bool compress{ false };
	bool virtualTexture{ false };
	uint32_t maxResolution{ 4096 };
	VkFilter magFilter{ VK_FILTER_LINEAR };
	VkFilter minFilter{ VK_FILTER_LINEAR };
	VkSamplerMipmapMode mipmapMode{ VK_SAMPLER_MIPMAP_MODE_LINEAR };
	VkSamplerAddressMode addressModeU{ VK_SAMPLER_ADDRESS_MODE_REPEAT };
	VkSamplerAddressMode addressModeV{ VK_SAMPLER_ADDRESS_MODE_REPEAT };
	float anisotropy{ 16.0f };
};

struct MeshImportSettings
{
	bool generateLODs{ true };
	uint32_t maxLODCount{ 4 };
	bool keepCpuPickingData{ true };
	bool optimizeVertexCache{ true };
	bool optimizeOverdraw{ true };
	bool virtualizeTextures{ true };
};

struct SubAssetEntry
{
	std::string name;
	UUID uuid;
	AssetType type{ AssetType::Unknown };
	bool virtualTexture{ false };
};

struct AssetMetadata
{
	UUID uuid;
	AssetType type{ AssetType::Unknown };

	std::string name;
	std::filesystem::path virtualPath;
	std::filesystem::path cookedPath;

	// compatibility aliases for existing code
	std::string id;
	std::filesystem::path path;

	UUID parentAssetUUID{}; // non-zero for subassets of a main asset
	std::vector<UUID> dependencies; // list of UUIDs of assets that this asset depends on
	std::vector<SubAssetEntry> subAssets; // subassets owned by this asset

	ResourceState state{ ResourceState::Unloaded };
	uint64_t diskSizeBytes{ 0 };
	uint64_t gpuSizeBytes{ 0 };
	uint64_t lastWriteTime{ 0 };
	std::string sourceHash;

	TextureImportSettings textureSettings{};
	MeshImportSettings meshSettings{};

	bool is_sub_asset() const noexcept { return parentAssetUUID.is_valid(); }
	bool is_loaded() const noexcept { return state == ResourceState::Loaded; }
};

using AssetRecord = AssetMetadata; // alias for compatibility with existing code

namespace AssetUtils
{
	std::string_view asset_type_to_string(AssetType type);
	AssetType asset_type_from_string(std::string_view str);
	AssetType asset_type_from_extension(const std::filesystem::path& path);
	std::string_view resource_state_to_string(ResourceState state);
}