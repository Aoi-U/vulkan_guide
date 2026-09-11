#pragma once

#include <utils/vk_types.h>
#include <unordered_map>
#include <filesystem>

#include "vk_descriptors.h"

// forward declaration
class VulkanEngine;

// represents a gltf material
struct GLTFMaterial
{
	MaterialInstance data;
};

// the bounding volume of a mesh used for frustum culling
struct Bounds
{
	glm::vec3 origin;
	float sphereRadius;
	glm::vec3 extents;
};

// a single surface of a mesh
struct GeoSurface
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

	std::vector<GeoSurface> surfaces;
	GPUMeshBuffers meshBuffers;
};

struct LoadedGLTF : public IRenderable
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

	VulkanEngine* creator = nullptr;

	glm::vec3 scenePosition{ 0.f };
	glm::vec3 sceneRotation{ 0.f };
	glm::vec3 sceneScale{ 1.f };
	bool lockScale{ true };

	~LoadedGLTF() { clearAll(); };

	virtual void draw(const glm::mat4& topMatrix, DrawContext& ctx);

private:
	void clearAll();
};

std::optional<std::shared_ptr<LoadedGLTF>> loadGltf(VulkanEngine* engine, std::string_view filePath);