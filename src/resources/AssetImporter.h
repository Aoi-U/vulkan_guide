#pragma once

#include <resources/AssetMetadata.h>
#include <resources/CookedFormats.h>
#include <filesystem>
#include <span>

#include "ResourceManager.h"

class AssetImporter
{
public:
	// cooks all registered disk assets if .gtex/.gmesh cache files are missing
	static void cook_missing_assets(); 

	// cooks image file into .cache/textures/<uuid>.gtex
	static bool cook_texture(const AssetMetadata& texMeta); 

	// cooks raw pixel memory into .gtex
	static bool write_gtex_file(const std::filesystem::path& cookedPath, const CookedTextureHeader& header, std::span<const uint8_t> pixelBytes);

	// cooks virtual texture into .cache/textures/<uuid>.vtex, 4px border padding across all mips
	static bool cook_virtual_texture(const AssetMetadata& texMeta);

	// slice raw RGBA8 mip0 image into padded 136x136 tiles across all mip levels and write to .vtx binary
	static bool cook_vtex_from_rgba8(const std::filesystem::path& cookedPath, const uint8_t* mip0Pixels, uint32_t width, uint32_t height, const TextureImportSettings& settings);

	// reads only the header and tile offset table from .vtex file
	static bool read_vtex_header(const std::filesystem::path& vtexPath, CookedVTexHeader& outHeader, std::vector<CookedVTexTileEntry>& outTileTable);

	// read single tile from .vtex file into outTilePixels
	static bool read_vtex_tile(const std::filesystem::path& vtexPath, const CookedVTexHeader& header, std::span<const CookedVTexTileEntry> tileTable, 
		uint32_t mipLevel, uint32_t tileX, uint32_t tileY, std::vector<uint8_t>& outTilePixels);

	// toggle single texture between .gtex and .vtex
	static bool set_texture_virtualized(const UUID& texUuid, bool enableVT, ResourceManager* rm = nullptr, bool saveMeta = true, bool forceRecook = false);

	// toggle all pbr textures of .mat between .gtex and .vtex
	static bool set_material_virtualized(const UUID& matUuid, bool enableVT, ResourceManager* rm = nullptr);

	// virtualize/unvirtualize all textures for an entire model/prefab
	static uint32_t set_model_virtualized(const UUID& modelOrPrefabUuid, bool enableVT, ResourceManager* rm = nullptr);

	// optimize raw vertex/index, generate 4 lods and write to .gmesh
	static bool cook_mesh_data(const std::filesystem::path& cookedPath, std::span<const Vertex> rawVertices, std::span<const uint32_t> rawIndices, const MeshImportSettings& settings, Bounds& outBounds);

	// creates .gtex, .mat, .gmesh, .prefab from .gltf/.glb
	static bool import_model(const AssetMetadata& modelMeta);
};
