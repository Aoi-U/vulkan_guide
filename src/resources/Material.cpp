#include "resources/Material.h"

#include <graphics/VulkanCore.h>
#include <utils/vk_initializers.h>

VkDescriptorSetLayout Material::_layout = VK_NULL_HANDLE;
DescriptorAllocatorGrowable Material::_descriptorPool;

Material::Material(const std::string& id, ResourceManager* manager, MaterialPassType passType) 
	: Resource(id), _resourceManager(manager), _passType(passType)
{}

void Material::set_texture(const TextureSlot slot, ResourceHandle<Texture> texture)
{
	switch (slot) {
	case TextureSlot::BaseColor:
		_baseColorTexture = texture;
		break;
	case TextureSlot::MetallicRoughness:
		_metalRoughTexture = texture;
		break;
	case TextureSlot::Occlusion:
		_metalRoughTexture = texture;
		break;
	}
}

void Material::set_constants(const MaterialConstants & constants)
{
	_constants = constants;
}

void Material::update_buffer(VulkanCore * core)
{
	if (!is_loaded()) return;

	memcpy(_materialBuffer.info.pMappedData, &_constants, sizeof(MaterialConstants));

	VkImageView baseColorView = _baseColorTexture ? _baseColorTexture->get_image_view() : core->_whiteImage.imageView;
	VkSampler baseColorSampler = _baseColorTexture ? _baseColorTexture->get_sampler() : core->_defaultSamplerLinear;

	VkImageView metalRoughView = _metalRoughTexture ? _metalRoughTexture->get_image_view() : core->_whiteImage.imageView;
	VkSampler metalRoughSampler = _metalRoughTexture ? _metalRoughTexture->get_sampler() : core->_defaultSamplerLinear;

	VkImageView occlusionView = _occlusionTexture ? _occlusionTexture->get_image_view() : core->_whiteImage.imageView;
	VkSampler occlusionSampler = _occlusionTexture ? _occlusionTexture->get_sampler() : core->_defaultSamplerLinear;

	// update descriptor set with the new buffer and textures
	DescriptorWriter writer;
	writer.write_buffer(0, _materialBuffer.buffer, sizeof(MaterialConstants), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	writer.write_image(1, baseColorView, baseColorSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.write_image(2, metalRoughView, metalRoughSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.write_image(3, occlusionView, occlusionSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.update_set(core->get_device(), _descriptorSet);
}

void Material::init_descriptor_layout(VulkanCore * core)
{
	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); // binding 0: uniform buffer for MaterialConstants
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 1: base color texture
	builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 2: metallic-roughness texture
	builder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 3: occlusion texture
	_layout = builder.build(core->get_device(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

	std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> poolSizes = {
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1.0 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3.0 }
	};
	_descriptorPool.init(core->get_device(), 1000, poolSizes);
}

void Material::cleanup_descriptor_layout(VulkanCore * core)
{
	if (_layout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(core->get_device(), _layout, nullptr);
		_layout = VK_NULL_HANDLE;
	}
	_descriptorPool.destroy_pools(core->get_device());
}

bool Material::doLoad(VulkanCore * core)
{
	_materialBuffer = core->create_buffer(sizeof(MaterialConstants), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	_descriptorSet = _descriptorPool.allocate(core->get_device(), _layout);
	update_buffer(core);

	return true;
}

bool Material::doUnload(VulkanCore* core)
{
	if (_materialBuffer.buffer != VK_NULL_HANDLE) {
		core->destroy_buffer(_materialBuffer);
		_materialBuffer = {};
	}

	// free descriptor set
	_descriptorSet = VK_NULL_HANDLE;

	return true;
}

