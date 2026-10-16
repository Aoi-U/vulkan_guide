#pragma once

#include <resources/AssetMetadata.h>
#include <resources/AssetMetadata.h>
#include <resources/MaterialInstance.h>
#include <graphics/utils/vk_types.h>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class ResourceManager;

struct MaterialAssetData
{
	UUID uuid{};
	std::string name{ "Unnamed material" };
	std::string templateId{ "pbr_default_template" };
	MaterialPassType passType{ MaterialPassType::Opaque };
	AlphaMode alphaMode{ AlphaMode::Opaque };
	bool doubleSided{ false };

	// pbr constants
	glm::vec4 colorFactors{ 1.0f, 1.0f, 1.0f, 1.0f };
	glm::vec4 metalRoughFactors{ 1.0f, 0.85f, 0.0f, 0.0f };
	float occlusionStrength{ 1.0f };
	float normalScale{ 1.0f };
	float transmissionFactor{ 0.0f };
	glm::vec3 emissiveFactor{ 0.0f, 0.0f, 0.0f };
	float emissiveStrength{ 1.0f };

	UUID albedoTextureUUID{};
	UUID normalTextureUUID{};
	UUID metalRoughTextureUUID{};
	UUID occlusionTextureUUID{};
	UUID emissiveTextureUUID{};
};

// disk representation of submesh binding inside .prefab node
struct PrefabSubmeshBinding
{
	UUID meshAssetUUID{};
	UUID materialAssetUUID{};
	uint32_t startIndex{ 0 };
	uint32_t indexCount{ 0 };
	Bounds bounds{};
};

// disk representation of a node inside .prefab file
struct PrefabNodeData
{
	std::string name{ "Node" };
	glm::vec3 position{ 0.0f };
	glm::vec3 rotation{ 0.0f };
	glm::vec3 scale{ 1.0f };
	int32_t parentIndex{ -1 };
	std::vector<uint32_t> childrenIndices;
	std::vector<PrefabSubmeshBinding> submeshes;
};

struct PrefabAssetData
{
	UUID uuid{};
	std::string name{ "Prefab" };
	UUID sourceModelUUID{};
	std::vector<PrefabNodeData> nodes;
};

class AssetSerializer
{
public:
	// .meta yaml serialization
	static bool save_meta_file(const std::filesystem::path& metaFilePath, const AssetMetadata& metadata);
	static bool load_meta_file(const std::filesystem::path& metaFilePath, AssetMetadata& outMetadata);

	// .mat yaml serialization
	static bool save_material_file(const std::filesystem::path& matFilePath, const MaterialAssetData& matData);
	static bool load_material_file(const std::filesystem::path& matFilePath, MaterialAssetData& outMatData);
	
	static std::shared_ptr<MaterialInstance> build_material_instance(const MaterialAssetData& matData, ResourceManager& rm);

	// .prefab yaml serialization
	static bool save_prefab_file(const std::filesystem::path& prefabFilePath, const PrefabAssetData& prefabData);
	static bool load_prefab_file(const std::filesystem::path& prefabFilePath, PrefabAssetData& outPrefabData);
};