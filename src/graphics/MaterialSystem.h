#pragma once

#include <core/vk_types.h>
#include <utils/vk_descriptors.h>

class VulkanCore;

// data that is sent to shader for a material instance
struct MaterialConstants
{
	glm::vec4 colorFactors;
	glm::vec4 metalRoughFactors;
	glm::vec4 extraFactors; // x: occlusionStrength
	// padding, needed for uniform bnuffers
	glm::vec4 padding[13];
};

// resources needed for a material instance
struct MaterialResources
{
	AllocatedImage colorImage;
	VkSampler colorSampler;

	AllocatedImage metalRoughImage;
	VkSampler metalRoughSampler;

	AllocatedImage occlusionImage;
	VkSampler occlusionSampler;

	VkBuffer dataBuffer;
	uint32_t dataBufferOffset;
};

class MaterialSystem
{
public:
	void init(VulkanCore* vulkanCore);
	void cleanup();

	VkDescriptorSetLayout get_material_layout() const { return _materialLayout; }

	MaterialInstance create_metallic_roughness_instance(MaterialPassType passType, const MaterialResources& resources, DescriptorAllocatorGrowable& descriptorAllocator);

private:
	VulkanCore* _vulkanCore;
	VkDescriptorSetLayout _materialLayout;
};