#pragma once

#include <resources/AssetMetadata.h>
#include <resources/CookedFormats.h>
#include <filesystem>
#include <span>

class AssetImporter
{
public:
	// cooks all registered disk assets if .gtex/.gmesh cache files are missing
	static void cook_missing_assets(); 

	// cooks image file into .cache/textures/<uuid>.gtex
	static bool cook_texture(const AssetMetadata& texMeta); 

	// cooks raw pixel memory into .gtex
	static bool write_gtex_file(const std::filesystem::path& cookedPath, const CookedTextureHeader& header, std::span<const uint8_t> pixelBytes);

	// optimize raw vertex/index, generate 4 lods and write to .gmesh
	static bool cook_mesh_data(const std::filesystem::path& cookedPath, std::span<const Vertex> rawVertices, std::span<const uint32_t> rawIndices, const MeshImportSettings& settings, Bounds& outBounds);

	// creates .gtex, .mat, .gmesh, .prefab from .gltf/.glb
	static bool import_model(const AssetMetadata& modelMeta);
};