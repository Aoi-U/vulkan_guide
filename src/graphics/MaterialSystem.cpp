#include "MaterialSystem.h"
#include <graphics/VulkanCore.h>
#include <utils/vk_initializers.h>
#include <utils/vk_pipelines.h>
#include <utils/vk_utils.h>

void MaterialSystem::init(VulkanCore* vulkanCore)
{
	_vulkanCore = vulkanCore;

	DescriptorLayoutBuilder layoutBuilder;
	layoutBuilder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); // binding 0: uniform buffer for MaterialConstants
	layoutBuilder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 1: combined image sampler for base color texture
	layoutBuilder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 2: combined image sampler for metallic-roughness texture
	layoutBuilder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 3: combined image sampler for occlusion texture 
	_materialLayout = layoutBuilder.build(_vulkanCore->get_device(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
}

void MaterialSystem::cleanup()
{
	if (_materialLayout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(_vulkanCore->get_device(), _materialLayout, nullptr);
		_materialLayout = VK_NULL_HANDLE;
	}
}

MaterialInstance MaterialSystem::create_metallic_roughness_instance(MaterialPassType passType, const MaterialResources & resources, DescriptorAllocatorGrowable & descriptorAllocator)
{
	MaterialInstance instance;
	instance.type = MaterialType::MetallicRoughness;
	instance.passType = passType;
	instance.materialSet = descriptorAllocator.allocate(_vulkanCore->get_device(), _materialLayout);

	DescriptorWriter writer;
	writer.write_buffer(0, resources.dataBuffer, sizeof(MaterialConstants), resources.dataBufferOffset, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	writer.write_image(1, resources.colorImage.imageView, resources.colorSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.write_image(2, resources.metalRoughImage.imageView, resources.metalRoughSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.write_image(3, resources.occlusionImage.imageView, resources.occlusionSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.update_set(_vulkanCore->get_device(), instance.materialSet);

	return instance;
}
