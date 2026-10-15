#pragma once

#include <resources/AssetMetadata.h>
#include <filesystem>

class AssetSerializer
{
public:
	// write AssetMetaData struct to .meta yaml file
	static bool save_meta_file(const std::filesystem::path& metaFilePath, const AssetMetadata& metadata);
	
	// read AssetMetaData struct from .meta yaml file
	static bool load_meta_file(const std::filesystem::path& metaFilePath, AssetMetadata& outMetadata);
};