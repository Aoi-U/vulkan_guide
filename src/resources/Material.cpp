#include "resources/Material.h"

#include <graphics/VulkanCore.h>
#include <utils/vk_initializers.h>
#include <resources/DefaultResources.h>

Material::Material(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, MaterialPassType passType)
	: Resource(id, core, resourceManager),  _passType(passType)
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
		_occlusionTexture = texture;
		break;
	case TextureSlot::Normal:
		_normalTexture = texture;
		break;
	case TextureSlot::Emissive:
		_emissiveTexture = texture;
		break;
	}
}

void Material::set_constants(const MaterialConstants & constants)
{
	_constants = constants;
}


ResourceHandle<Texture> Material::get_texture(TextureSlot slot) const
{
	switch (slot) {
	case TextureSlot::BaseColor:          
		return _baseColorTexture;
	case TextureSlot::MetallicRoughness:  
		return _metalRoughTexture;
	case TextureSlot::Occlusion:          
		return _occlusionTexture;
	case TextureSlot::Normal:             
		return _normalTexture;
	case TextureSlot::Emissive:           
		return _emissiveTexture;
	default:                              
		return {};
	}
}
