#pragma once

#include <core/vk_types.h>
#include <utils/vk_descriptors.h>

class VulkanCore;

// data that is sent to shader for a material instance
struct MaterialConstants
{
	glm::vec4 colorFactors;
	glm::vec4 metalRoughFactors;
	// padding, needed for uniform bnuffers
	glm::vec4 padding[14];
};

// resources needed for a material instance
struct MaterialResources
{
	AllocatedImage colorImage;
	VkSampler colorSampler;
	AllocatedImage metalRoughImage;
	VkSampler metalRoughSampler;
	VkBuffer dataBuffer;
	uint32_t dataBufferOffset;
};

// a gltf material with metallic-roughness
class GLTFMetallic_Roughness
{
public:
	MaterialPipeline opaquePipeline;
	MaterialPipeline transparentPipeline;

	VkDescriptorSetLayout materialLayout;

	// build the pipelines for this material
	void build_pipelines(VkDevice device, VkDescriptorSetLayout sceneDataLayout, VkFormat colorFormat, VkFormat depthFormat);
	void clear_resources(VkDevice device);

	// create a material instance with its own resources
	MaterialInstance write_material(VkDevice device, MaterialPass pass, const MaterialResources& resources, DescriptorAllocatorGrowable& descriptorAllocator);

private:
	DescriptorWriter writer;
};