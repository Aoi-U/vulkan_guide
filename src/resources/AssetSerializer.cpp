#include "AssetSerializer.h"

#include <yaml-cpp/yaml.h>
#include <fmt/color.h>
#include <fstream>
#include <algorithm>

namespace
{
	std::string_view filter_to_string(VkFilter filter)
	{
		return (filter == VK_FILTER_NEAREST) ? "NEAREST" : "LINEAR";
	}

	VkFilter filter_from_string(std::string_view str)
	{
		return (str == "NEAREST") ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
	}

	std::string_view mipmap_to_string(VkSamplerMipmapMode mode)
	{
		return (mode == VK_SAMPLER_MIPMAP_MODE_NEAREST) ? "NEAREST" : "LINEAR";
	}

	VkSamplerMipmapMode mipmap_from_string(std::string_view str)
	{
		return (str == "NEAREST") ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
	}

	std::string_view address_to_string(VkSamplerAddressMode mode)
	{
		switch (mode) {
		case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:
			return "CLAMP_TO_EDGE";
		case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:
			return "CLAMP_TO_BORDER";
		case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:
			return "MIRRORED_REPEAT";
		default:
			return "REPEAT";
		}
	}

	VkSamplerAddressMode address_from_string(std::string_view str)
	{
		if (str == "CLAMP_TO_EDGE")
			return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		if (str == "CLAMP_TO_BORDER")
			return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		if (str == "MIRRORED_REPEAT")
			return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
		return VK_SAMPLER_ADDRESS_MODE_REPEAT;
	}
}

namespace AssetUtils
{
	std::string_view asset_type_to_string(AssetType type)
	{
		switch (type) {
		case AssetType::Texture:        
			return "Texture";
		case AssetType::Model:          
			return "Model";
		case AssetType::Mesh:           
			return "Mesh";
		case AssetType::Material:       
			return "Material";
		case AssetType::Shader:         
			return "Shader";
		case AssetType::EnvironmentHDR: 
			return "EnvironmentHDR";
		case AssetType::Scene:          
			return "Scene";
		case AssetType::Prefab:         
			return "Prefab";
		default:                        
			return "Unknown";
		}
	}

	AssetType asset_type_from_string(std::string_view str)
	{
		if (str == "Texture")        
			return AssetType::Texture;
		if (str == "Model")          
			return AssetType::Model;
		if (str == "Mesh")           
			return AssetType::Mesh;
		if (str == "Material")       
			return AssetType::Material;
		if (str == "Shader")         
			return AssetType::Shader;
		if (str == "EnvironmentHDR") 
			return AssetType::EnvironmentHDR;
		if (str == "Scene")          
			return AssetType::Scene;
		if (str == "Prefab")         
			return AssetType::Prefab;
		return AssetType::Unknown;
	}

	AssetType asset_type_from_extension(const std::filesystem::path& path)
	{
		std::string ext = path.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

		if (ext == ".glb" || ext == ".gltf")
			return AssetType::Model;
		if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".gtex")
			return AssetType::Texture;
		if (ext == ".gmesh")
			return AssetType::Mesh;
		if (ext == ".mat")
			return AssetType::Material;
		if (ext == ".hdr")
			return AssetType::EnvironmentHDR;
		if (ext == ".vert" || ext == ".frag" || ext == ".comp" || ext == ".spv")
			return AssetType::Shader;
		if (ext == ".scene")
			return AssetType::Scene;
		if (ext == ".prefab")
			return AssetType::Prefab;

		return AssetType::Unknown;
	}

	std::string_view resource_state_to_string(ResourceState state)
	{
		switch (state) {
		case ResourceState::Unloaded: 
			return "Unloaded";
		case ResourceState::Queued:   
			return "Queued";
		case ResourceState::Loading:  
			return "Loading";
		case ResourceState::Loaded:   
			return "Loaded";
		case ResourceState::Failed:   
			return "Failed";
		default:                      
			return "Unknown";
		}
	}
}

bool AssetSerializer::save_meta_file(const std::filesystem::path& metaFilePath, const AssetMetadata& metadata)
{
	YAML::Emitter out;
	out << YAML::BeginMap;
	out << YAML::Key << "uuid" << YAML::Value << metadata.uuid;
	out << YAML::Key << "type" << YAML::Value << std::string(AssetUtils::asset_type_to_string(metadata.type));
	out << YAML::Key << "name" << YAML::Value << metadata.name;
	out << YAML::Key << "version" << YAML::Value << 1;
	out << YAML::Key << "lastWriteTime" << YAML::Value << metadata.lastWriteTime;
	out << YAML::Key << "sourceHash" << YAML::Value << metadata.sourceHash;

	if (!metadata.cookedPath.empty()) 
		out << YAML::Key << "cookedPath" << YAML::Value << metadata.cookedPath.generic_string();
	
	if (metadata.parentAssetUUID.is_valid())
		out << YAML::Key << "parentAssetUUID" << YAML::Value << metadata.parentAssetUUID;

	// resource type specific import settings
	if (metadata.type == AssetType::Texture || metadata.type == AssetType::EnvironmentHDR) {
		const auto& ts = metadata.textureSettings;
		out << YAML::Key << "importSettings" << YAML::Value << YAML::BeginMap;
		out << YAML::Key << "sRGB" << YAML::Value << ts.sRGB;
		out << YAML::Key << "generateMips" << YAML::Value << ts.generateMips;
		out << YAML::Key << "compressBC7" << YAML::Value << ts.compress;
		out << YAML::Key << "maxResolution" << YAML::Value << ts.maxResolution;
		out << YAML::Key << "sampler" << YAML::Value << YAML::BeginMap;
		out << YAML::Key << "magFilter" << YAML::Value << std::string(filter_to_string(ts.magFilter));
		out << YAML::Key << "minFilter" << YAML::Value << std::string(filter_to_string(ts.minFilter));
		out << YAML::Key << "mipmapMode" << YAML::Value << std::string(mipmap_to_string(ts.mipmapMode));
		out << YAML::Key << "addressModeU" << YAML::Value << std::string(address_to_string(ts.addressModeU));
		out << YAML::Key << "addressModeV" << YAML::Value << std::string(address_to_string(ts.addressModeV));
		out << YAML::Key << "anisotropy" << YAML::Value << ts.anisotropy;
		out << YAML::EndMap; // sampler
		out << YAML::EndMap; // importSettings
	}
	else if (metadata.type == AssetType::Model || metadata.type == AssetType::Mesh) {
		const auto& ms = metadata.meshSettings;
		out << YAML::Key << "importSettings" << YAML::Value << YAML::BeginMap;
		out << YAML::Key << "generateLODs" << YAML::Value << ms.generateLODs;
		out << YAML::Key << "maxLODCount" << YAML::Value << ms.maxLODCount;
		out << YAML::Key << "keepCpuPickingData" << YAML::Value << ms.keepCpuPickingData;
		out << YAML::Key << "optimizeVertexCache" << YAML::Value << ms.optimizeVertexCache;
		out << YAML::Key << "optimizeOverdraw" << YAML::Value << ms.optimizeOverdraw;
		out << YAML::EndMap; // importSettings
	}

	if (!metadata.dependencies.empty()) {
		out << YAML::Key << "dependencies" << YAML::Value << YAML::BeginSeq;
		for (const auto& dep : metadata.dependencies) {
			out << dep;
		}
		out << YAML::EndSeq;
	}

	if (!metadata.subAssets.empty()) {
		out << YAML::Key << "subAssets" << YAML::Value << YAML::BeginSeq;
		for (const auto& sub : metadata.subAssets) {
			out << YAML::BeginMap;
			out << YAML::Key << "name" << YAML::Value << sub.name;
			out << YAML::Key << "uuid" << YAML::Value << sub.uuid;
			out << YAML::Key << "type" << YAML::Value << std::string(AssetUtils::asset_type_to_string(sub.type));
			out << YAML::EndMap;
		}
		out << YAML::EndSeq;
	}

	out << YAML::EndMap;

	// write to file
	std::ofstream fout(metaFilePath);
	if (!fout.is_open()) {
		fmt::println("AssetSerializer: failed to write meta file: {}", metaFilePath.string());
		return false;
	}

	fout << out.c_str();
	return true;
}

bool AssetSerializer::load_meta_file(const std::filesystem::path& metaFilePath, AssetMetadata& outMetadata)
{
	if (!std::filesystem::exists(metaFilePath))
		return false;

	try {
		YAML::Node root = YAML::LoadFile(metaFilePath.string());

		if (!root["uuid"]) 
			return false;

		outMetadata.uuid = root["uuid"].as<UUID>();
		if (root["type"]) 
			outMetadata.type = AssetUtils::asset_type_from_string(root["type"].as<std::string>());
		
		if (root["name"]) 
			outMetadata.name = root["name"].as<std::string>();
		
		if (root["lastWriteTime"]) 
			outMetadata.lastWriteTime = root["lastWriteTime"].as<uint64_t>();
		
		if (root["sourceHash"]) 
			outMetadata.sourceHash = root["sourceHash"].as<std::string>();
		
		if (root["cookedPath"]) 
			outMetadata.cookedPath = root["cookedPath"].as<std::string>();
		
		if (root["parentAssetUUID"]) 
			outMetadata.parentAssetUUID = root["parentAssetUUID"].as<UUID>();
		

		if (YAML::Node settings = root["importSettings"]) {
			if (outMetadata.type == AssetType::Texture || outMetadata.type == AssetType::EnvironmentHDR) {
				auto& ts = outMetadata.textureSettings;
				if (settings["sRGB"])          ts.sRGB = settings["sRGB"].as<bool>();
				if (settings["generateMips"])  ts.generateMips = settings["generateMips"].as<bool>();
				if (settings["compressBC7"])   ts.compress = settings["compressBC7"].as<bool>();
				if (settings["maxResolution"]) ts.maxResolution = settings["maxResolution"].as<uint32_t>();

				if (YAML::Node sampler = settings["sampler"]) {
					if (sampler["magFilter"])    ts.magFilter = filter_from_string(sampler["magFilter"].as<std::string>());
					if (sampler["minFilter"])    ts.minFilter = filter_from_string(sampler["minFilter"].as<std::string>());
					if (sampler["mipmapMode"])   ts.mipmapMode = mipmap_from_string(sampler["mipmapMode"].as<std::string>());
					if (sampler["addressModeU"]) ts.addressModeU = address_from_string(sampler["addressModeU"].as<std::string>());
					if (sampler["addressModeV"]) ts.addressModeV = address_from_string(sampler["addressModeV"].as<std::string>());
					if (sampler["anisotropy"])   ts.anisotropy = sampler["anisotropy"].as<float>();
				}
			}
			else if (outMetadata.type == AssetType::Model || outMetadata.type == AssetType::Mesh) {
				auto& ms = outMetadata.meshSettings;
				if (settings["generateLODs"])        ms.generateLODs = settings["generateLODs"].as<bool>();
				if (settings["maxLODCount"])         ms.maxLODCount = settings["maxLODCount"].as<uint32_t>();
				if (settings["keepCpuPickingData"])  ms.keepCpuPickingData = settings["keepCpuPickingData"].as<bool>();
				if (settings["optimizeVertexCache"]) ms.optimizeVertexCache = settings["optimizeVertexCache"].as<bool>();
				if (settings["optimizeOverdraw"])    ms.optimizeOverdraw = settings["optimizeOverdraw"].as<bool>();
			}
		}

		outMetadata.dependencies.clear();
		if (YAML::Node deps = root["dependencies"]; deps && deps.IsSequence()) {
			for (const auto& depNode : deps) {
				outMetadata.dependencies.push_back(depNode.as<UUID>());
			}
		}

		outMetadata.subAssets.clear();
		if (YAML::Node subs = root["subAssets"]; subs && subs.IsSequence()) {
			for (const auto& subNode : subs) {
				SubAssetEntry entry{};
				entry.name = subNode["name"].as<std::string>("");
				entry.uuid = subNode["uuid"].as<UUID>();
				entry.type = AssetUtils::asset_type_from_string(subNode["type"].as<std::string>("Unknown"));
				outMetadata.subAssets.push_back(std::move(entry));
			}
		}

		return outMetadata.uuid.is_valid();
	}
	catch (const YAML::Exception& e) {
		fmt::println("AssetSerializer: YAML error reading meta file '{}': {}", metaFilePath.string(), e.what());
		return false;
	}
}
