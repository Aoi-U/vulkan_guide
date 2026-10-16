#include "AssetSerializer.h"

#include <resources/DefaultResources.h>
#include <resources/ResourceManager.h>
#include <core/YamlConverters.h>
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

	std::string_view pass_type_to_string(MaterialPassType pass)
	{
		return (pass == MaterialPassType::Transparent) ? "Transparent" : "Opaque";
	}

	MaterialPassType pass_type_from_string(std::string_view str)
	{
		if (str == "Transparent") return MaterialPassType::Transparent;
		return MaterialPassType::Opaque;
	}

	std::string_view alpha_mode_to_string(AlphaMode mode)
	{
		switch (mode) {
		case AlphaMode::Mask:  return "Mask";
		case AlphaMode::Blend: return "Blend";
		default:               return "Opaque";
		}
	}

	AlphaMode alpha_mode_from_string(std::string_view str)
	{
		if (str == "Mask")  return AlphaMode::Mask;
		if (str == "Blend") return AlphaMode::Blend;
		return AlphaMode::Opaque;
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

bool AssetSerializer::save_material_file(const std::filesystem::path& matFilePath, const MaterialAssetData& matData)
{
	if (matFilePath.has_parent_path()) {
		std::filesystem::create_directories(matFilePath.parent_path());
	}

	// write material data to YAML
	YAML::Emitter out;
	out << YAML::BeginMap;
	out << YAML::Key << "uuid" << YAML::Value << (matData.uuid.is_valid() ? matData.uuid : UUID::generate());
	out << YAML::Key << "type" << YAML::Value << "Material";
	out << YAML::Key << "name" << YAML::Value << matData.name;
	out << YAML::Key << "templateId" << YAML::Value << matData.templateId;
	out << YAML::Key << "passType" << YAML::Value << std::string(pass_type_to_string(matData.passType));
	out << YAML::Key << "alphaMode" << YAML::Value << std::string(alpha_mode_to_string(matData.alphaMode));
	out << YAML::Key << "doubleSided" << YAML::Value << matData.doubleSided;

	out << YAML::Key << "constants" << YAML::Value << YAML::BeginMap;
	out << YAML::Key << "colorFactors" << YAML::Value << matData.colorFactors;
	out << YAML::Key << "metalRoughFactors" << YAML::Value << matData.metalRoughFactors;
	out << YAML::Key << "occlusionStrength" << YAML::Value << matData.occlusionStrength;
	out << YAML::Key << "normalScale" << YAML::Value << matData.normalScale;
	out << YAML::Key << "transmissionFactor" << YAML::Value << matData.transmissionFactor;
	out << YAML::Key << "emissiveFactor" << YAML::Value << matData.emissiveFactor;
	out << YAML::Key << "emissiveStrength" << YAML::Value << matData.emissiveStrength;
	out << YAML::EndMap;

	auto emit_tex_uuid = [&](const char* slotName, const UUID& texUuid) {
		out << YAML::Key << slotName;
		if (texUuid.is_valid()) {
			out << YAML::Value << texUuid;
		}
		else {
			out << YAML::Value << "0";
		}
		};

	out << YAML::Key << "textures" << YAML::Value << YAML::BeginMap;
	emit_tex_uuid("Albedo", matData.albedoTextureUUID);
	emit_tex_uuid("Normal", matData.normalTextureUUID);
	emit_tex_uuid("MetalRough", matData.metalRoughTextureUUID);
	emit_tex_uuid("Occlusion", matData.occlusionTextureUUID);
	emit_tex_uuid("Emissive", matData.emissiveTextureUUID);
	out << YAML::EndMap;

	out << YAML::EndMap;

	// write to file
	std::ofstream fout(matFilePath);
	if (!fout.is_open()) {
		fmt::println("AssetSerializer: Failed to write material file: {}", matFilePath.string());
		return false;
	}
	fout << out.c_str();

	return true;
}

bool AssetSerializer::load_material_file(const std::filesystem::path& matFilePath, MaterialAssetData& outMatData)
{
	if (!std::filesystem::exists(matFilePath)) {
		fmt::println("AssetSerializer: Material file does not exist: {}", matFilePath.string());
		return false;
	}

	try { // parse YAML
		YAML::Node root = YAML::LoadFile(matFilePath.string());
		if (root["uuid"])        outMatData.uuid = root["uuid"].as<UUID>();
		if (root["name"])        outMatData.name = root["name"].as<std::string>();
		if (root["templateId"])  outMatData.templateId = root["templateId"].as<std::string>();
		if (root["passType"])    outMatData.passType = pass_type_from_string(root["passType"].as<std::string>());
		if (root["alphaMode"])   outMatData.alphaMode = alpha_mode_from_string(root["alphaMode"].as<std::string>());
		if (root["doubleSided"]) outMatData.doubleSided = root["doubleSided"].as<bool>();

		if (YAML::Node constants = root["constants"]) {
			if (constants["colorFactors"])       outMatData.colorFactors = constants["colorFactors"].as<glm::vec4>();
			if (constants["metalRoughFactors"])  outMatData.metalRoughFactors = constants["metalRoughFactors"].as<glm::vec4>();
			if (constants["occlusionStrength"])  outMatData.occlusionStrength = constants["occlusionStrength"].as<float>();
			if (constants["normalScale"])        outMatData.normalScale = constants["normalScale"].as<float>();
			if (constants["transmissionFactor"]) outMatData.transmissionFactor = constants["transmissionFactor"].as<float>();
			if (constants["emissiveFactor"])     outMatData.emissiveFactor = constants["emissiveFactor"].as<glm::vec3>();
			if (constants["emissiveStrength"])   outMatData.emissiveStrength = constants["emissiveStrength"].as<float>();
		}

		if (YAML::Node textures = root["textures"]) {
			if (textures["Albedo"])     outMatData.albedoTextureUUID = textures["Albedo"].as<UUID>();
			if (textures["Normal"])     outMatData.normalTextureUUID = textures["Normal"].as<UUID>();
			if (textures["MetalRough"]) outMatData.metalRoughTextureUUID = textures["MetalRough"].as<UUID>();
			if (textures["Occlusion"])  outMatData.occlusionTextureUUID = textures["Occlusion"].as<UUID>();
			if (textures["Emissive"])   outMatData.emissiveTextureUUID = textures["Emissive"].as<UUID>();
		}

		return true;
	}
	catch (const YAML::Exception& e) {
		fmt::println("AssetSerializer: YAML error reading material '{}': {}", matFilePath.string(), e.what());
		return false;
	}
}

std::shared_ptr<MaterialInstance> AssetSerializer::build_material_instance(const MaterialAssetData& matData, ResourceManager& rm)
{
	auto& defaults = DefaultResources::get();
	ResourceHandle<MaterialTemplate> matTemplate = rm.get_handle<MaterialTemplate>(matData.templateId);
	if (!matTemplate.is_valid())
		matTemplate = defaults.defaultPBRTemplate;

	// create material instance and set parameters
	auto instance = std::make_shared<MaterialInstance>(matTemplate);
	instance->set_alpha_mode(matData.alphaMode);
	instance->set_double_sided(matData.doubleSided);

	// set constants
	instance->set_parameter("colorFactors", matData.colorFactors);
	instance->set_parameter("metalRoughFactors", matData.metalRoughFactors);
	glm::vec4 extraFactors(
		matData.occlusionStrength,
		matData.normalScale,
		matData.doubleSided ? 1.0f : 0.0f,
		matData.transmissionFactor
	);
	instance->set_parameter("extraFactors", extraFactors);
	instance->set_parameter("emissiveFactor", glm::vec4(matData.emissiveFactor, matData.emissiveStrength));

	// helper to resolve texture uuid to  gpu handle
	auto resolve_slot_texture = [&](const UUID& texUuid, ResourceHandle<Texture> fallback, VkFormat defaultFormat) -> ResourceHandle<Texture> {
		if (!texUuid.is_valid()) {
			return fallback;
		}
		if (rm.has_resource<Texture>(texUuid)) {
			return rm.get_handle<Texture>(texUuid);
		}
		if (const AssetMetadata* meta = AssetRegistry::get().get_metadata(texUuid)) {
			VkFormat fmt = meta->textureSettings.sRGB ? VK_FORMAT_R8G8B8A8_SRGB : defaultFormat;
			auto loaded = rm.load_by_uuid<Texture>(
				texUuid,
				meta->name,
				meta->virtualPath,
				fmt,
				VK_IMAGE_USAGE_SAMPLED_BIT,
				meta->textureSettings.generateMips
			);
			if (loaded.is_valid()) return loaded;
		}
		return fallback;
		};

	instance->set_texture("albedo", resolve_slot_texture(matData.albedoTextureUUID, defaults.whiteImage, VK_FORMAT_R8G8B8A8_SRGB));
	instance->set_texture("normal", resolve_slot_texture(matData.normalTextureUUID, defaults.defaultNormalImage, VK_FORMAT_R8G8B8A8_UNORM));
	instance->set_texture("metalRough", resolve_slot_texture(matData.metalRoughTextureUUID, defaults.whiteImage, VK_FORMAT_R8G8B8A8_UNORM));
	instance->set_texture("occlusion", resolve_slot_texture(matData.occlusionTextureUUID, defaults.whiteImage, VK_FORMAT_R8G8B8A8_UNORM));
	instance->set_texture("emissive", resolve_slot_texture(matData.emissiveTextureUUID, defaults.blackImage, VK_FORMAT_R8G8B8A8_SRGB));

	return instance;
}

bool AssetSerializer::save_prefab_file(const std::filesystem::path& prefabFilePath, const PrefabAssetData& prefabData)
{
	if (prefabFilePath.has_parent_path()) {
		std::filesystem::create_directories(prefabFilePath.parent_path());
	}

	// write prefab data to YAML
	YAML::Emitter out;
	out << YAML::BeginMap;
	out << YAML::Key << "uuid" << YAML::Value << (prefabData.uuid.is_valid() ? prefabData.uuid : UUID::generate());
	out << YAML::Key << "type" << YAML::Value << "Prefab";
	out << YAML::Key << "name" << YAML::Value << prefabData.name;
	out << YAML::Key << "version" << YAML::Value << 1;
	out << YAML::Key << "sourceModelUUID" << YAML::Value << prefabData.sourceModelUUID;

	out << YAML::Key << "nodes" << YAML::Value << YAML::BeginSeq;
	for (const auto& node : prefabData.nodes) {
		out << YAML::BeginMap;
		out << YAML::Key << "name" << YAML::Value << node.name;
		out << YAML::Key << "parentIndex" << YAML::Value << node.parentIndex;
		out << YAML::Key << "position" << YAML::Value << node.position;
		out << YAML::Key << "rotation" << YAML::Value << node.rotation;
		out << YAML::Key << "scale" << YAML::Value << node.scale;

		out << YAML::Key << "childrenIndices" << YAML::Value << YAML::Flow << node.childrenIndices;

		if (!node.submeshes.empty()) {
			out << YAML::Key << "submeshes" << YAML::Value << YAML::BeginSeq;
			for (const auto& sm : node.submeshes) {
				out << YAML::BeginMap;
				out << YAML::Key << "meshAssetUUID" << YAML::Value << sm.meshAssetUUID;
				out << YAML::Key << "materialAssetUUID" << YAML::Value << sm.materialAssetUUID;
				out << YAML::Key << "startIndex" << YAML::Value << sm.startIndex;
				out << YAML::Key << "indexCount" << YAML::Value << sm.indexCount;
				out << YAML::Key << "bounds" << YAML::Value << YAML::BeginMap;
				out << YAML::Key << "origin" << YAML::Value << sm.bounds.origin;
				out << YAML::Key << "sphereRadius" << YAML::Value << sm.bounds.sphereRadius;
				out << YAML::Key << "extents" << YAML::Value << sm.bounds.extents;
				out << YAML::EndMap;
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;
		}
		out << YAML::EndMap;
	}
	out << YAML::EndSeq;
	out << YAML::EndMap;

	// write to file
	std::ofstream fout(prefabFilePath);
	if (!fout.is_open()) {
		fmt::println("AssetSerializer: Failed to write prefab file: {}", prefabFilePath.string());
		return false;
	}
	fout << out.c_str();

	return true;
}

bool AssetSerializer::load_prefab_file(const std::filesystem::path& prefabFilePath, PrefabAssetData& outPrefabData)
{
	if (!std::filesystem::exists(prefabFilePath)) return false;

	try { // parse YAML
		YAML::Node root = YAML::LoadFile(prefabFilePath.string());
		if (root["uuid"])            
			outPrefabData.uuid = root["uuid"].as<UUID>();
		if (root["name"])            
			outPrefabData.name = root["name"].as<std::string>();
		if (root["sourceModelUUID"]) 
			outPrefabData.sourceModelUUID = root["sourceModelUUID"].as<UUID>();

		outPrefabData.nodes.clear();
		if (YAML::Node nodes = root["nodes"]; nodes && nodes.IsSequence()) {
			for (const auto& n : nodes) {
				PrefabNodeData nodeData{};
				nodeData.name = n["name"].as<std::string>("Node");
				nodeData.parentIndex = n["parentIndex"].as<int32_t>(-1);

				if (n["position"]) 
					nodeData.position = n["position"].as<glm::vec3>();
				if (n["rotation"]) 
					nodeData.rotation = n["rotation"].as<glm::vec3>();
				if (n["scale"])    
					nodeData.scale = n["scale"].as<glm::vec3>();

				if (YAML::Node children = n["childrenIndices"]; children && children.IsSequence()) {
					for (const auto& cIdx : children) {
						nodeData.childrenIndices.push_back(cIdx.as<uint32_t>());
					}
				}

				if (YAML::Node submeshes = n["submeshes"]; submeshes && submeshes.IsSequence()) {
					for (const auto& smNode : submeshes) {
						PrefabSubmeshBinding sm{};
						if (smNode["meshAssetUUID"])     
							sm.meshAssetUUID = smNode["meshAssetUUID"].as<UUID>();
						if (smNode["materialAssetUUID"]) 
							sm.materialAssetUUID = smNode["materialAssetUUID"].as<UUID>();
						if (smNode["startIndex"])        
							sm.startIndex = smNode["startIndex"].as<uint32_t>();
						if (smNode["indexCount"])        
							sm.indexCount = smNode["indexCount"].as<uint32_t>();
						if (smNode["bounds"]) 
							sm.bounds = smNode["bounds"].as<Bounds>();
						nodeData.submeshes.push_back(sm);
					}
				}
				outPrefabData.nodes.push_back(std::move(nodeData));
			}
		}
		return true;
	}
	catch (const YAML::Exception& e) {
		fmt::println("AssetSerializer: YAML error reading prefab '{}': {}", prefabFilePath.string(), e.what());
		return false;
	}
}


