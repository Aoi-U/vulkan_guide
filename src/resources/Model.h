#pragma once

#include <graphics/utils/vk_types.h>
#include <resources/ResourceManager.h>
#include <resources/Mesh.h>
#include <resources/Material.h>
#include <filesystem>
#include <vector>
#include <memory>
#include <string>

namespace fastgltf {
	class Asset;
	struct Sampler;
}

struct ModelSubMesh
{
	uint32_t startIndex;
	uint32_t count;
	Bounds bounds;
	ResourceHandle<Mesh> mesh;
	ResourceHandle<Material> material;
};

struct ModelNode : public Node
{
	std::vector<ModelSubMesh> submeshes;
};

class Model : public Resource
{
public:
	Model(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, const std::filesystem::path& path);
	~Model() override { Resource::unload(); }

	std::vector<std::shared_ptr<ModelNode>> rootNodes;
	std::vector<std::shared_ptr<Node>> allNodes;

protected:
	bool doLoad() override;
	bool doUnload() override;

private:
	struct RawImageData 
	{
		int width{ 0 }, height{ 0 }, channels{ 0 };
		unsigned char* pixels{ nullptr };
		VkFormat format{ VK_FORMAT_R8G8B8A8_UNORM };

		VkFilter magFilter{ VK_FILTER_LINEAR };
		VkFilter minFilter{ VK_FILTER_LINEAR };
		VkSamplerMipmapMode mipmapMode{ VK_SAMPLER_MIPMAP_MODE_LINEAR };
		VkSamplerAddressMode addressModeU{ VK_SAMPLER_ADDRESS_MODE_REPEAT };
		VkSamplerAddressMode addressModeV{ VK_SAMPLER_ADDRESS_MODE_REPEAT };
	};

	struct ModelStagingUploadContext
	{
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
		GeometryAllocation geomAlloc;
		std::vector<std::vector<ModelSubMesh>> meshSubmeshes;
		std::vector<std::vector<glm::vec3>> meshPickingPositions;
		std::vector<std::vector<uint32_t>> meshPickingIndices;
	};

	bool parse_gltf_file(fastgltf::Asset& gltf);
	bool extract_textures(const fastgltf::Asset& gltf, std::vector<RawImageData>& rawImages);
	void extract_geometry(const fastgltf::Asset& gltf, ModelStagingUploadContext& uploadContext);
	void upload_gpu_resources(const fastgltf::Asset& gltf, std::vector<RawImageData>& rawImages, ModelStagingUploadContext& uploadContext);
	void create_materials(const fastgltf::Asset& gltf, const ModelStagingUploadContext& uploadContext);
	void create_meshes(const fastgltf::Asset& gltf, const ModelStagingUploadContext& uploadContext);
	void build_scene_hierarchy(const fastgltf::Asset& gltf, const ModelStagingUploadContext& uploadContext);

	std::filesystem::path _filePath;

	std::vector<ResourceHandle<Texture>> _textures;
	std::vector<ResourceHandle<Material>> _materials;

};
