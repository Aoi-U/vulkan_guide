#pragma once

#include <core/vk_types.h>
#include <utils/vk_descriptors.h>
#include <unordered_map>
#include <filesystem>
#include <memory>

// forward declaration
class VulkanCore;
class MaterialSystem;

// represents a gltf material
struct GLTFMaterial
{
	MaterialInstance data;
};

// a single submesh of a mesh
struct SubMesh
{
	uint32_t startIndex;
	uint32_t count;
	Bounds bounds;
	std::shared_ptr<GLTFMaterial> material;
};

// a mesh asset
struct MeshAsset
{
	std::string name;

	std::vector<SubMesh> surfaces;
	GPUMeshBuffers meshBuffers;
};

// scene graph node for a renderable mesh
struct MeshNode : public Node
{
	std::shared_ptr<MeshAsset> mesh;
};

struct LoadedGLTF 
{
	// storage for all the data on a given glTF file
	std::unordered_map<std::string, std::shared_ptr<MeshAsset>> meshes;
	std::unordered_map<std::string, std::shared_ptr<Node>> nodes;
	std::unordered_map<std::string, AllocatedImage> images;
	std::unordered_map<std::string, std::shared_ptr<GLTFMaterial>> materials;

	// ndoes that dont have a parent, for iterating through the file in tree order
	std::vector<std::shared_ptr<Node>> topNodes;
	
	std::vector<VkSampler> samplers;
	
	DescriptorAllocatorGrowable descriptorPool;

	AllocatedBuffer materialDataBuffer{};

	VulkanCore* creator = nullptr;

	~LoadedGLTF() { clearAll(); };

private:
	void clearAll();
};

std::optional<std::shared_ptr<LoadedGLTF>> loadGltf(VulkanCore* vulkanCore, MaterialSystem& materialSystem, std::string_view filePath);