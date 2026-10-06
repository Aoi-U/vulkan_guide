#include "resources/Texture.h"
#include <graphics/VulkanCore.h>
#include <fmt/core.h>
#include <stb_image.h>
#include <cmath>
#include <algorithm>

Texture::Texture(const std::string& id, const std::filesystem::path& filepath, VkFormat format, VkImageUsageFlags usage, bool generateMips)
  : Resource(id), _filePath(filepath), _format(format), _usage(usage), _mipmapped(generateMips) {
  if (_filePath.empty()) {
    _filePath = std::filesystem::path("assets/textures") / (id + ".png");
  }
}

Texture::Texture(const std::string& id, std::span<const uint8_t> data, VkFormat format, VkImageUsageFlags usage, bool generateMips)
	: Resource(id), _embeddedData(data.begin(), data.end()), _format(format), _usage(usage), _mipmapped(generateMips) 
{}

bool Texture::doLoad(VulkanCore * core)
{
	int width, height, channels;
	stbi_uc* pixels = nullptr;

	if (!_embeddedData.empty()) {
		pixels = stbi_load_from_memory(_embeddedData.data(), static_cast<int>(_embeddedData.size()), &width, &height, &channels, STBI_rgb_alpha);
		_embeddedData.clear(); // free cpu memory
	}
	else {
		pixels = stbi_load(_filePath.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
	}

	_extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
	_mipLevels = _mipmapped ? static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1 : 1;

	AllocatedImage allocated = core->create_image(pixels, _extent, _format, _usage, _mipmapped);

	stbi_image_free(pixels);

	if (!allocated.image) {
		fmt::print("Texture: failed to create vulkan image for: {}", _filePath.string());
		return false;
	}

	_image = allocated.image;
	_imageView = allocated.imageView;
	_allocation = allocated.allocation;

	// create a sampler
	VkSamplerCreateInfo samplerInfo = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.minLod = 0.0f;
	samplerInfo.maxLod = static_cast<float>(_mipLevels);
	samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
	samplerInfo.unnormalizedCoordinates = VK_FALSE;

	if (vkCreateSampler(core->get_device(), &samplerInfo, nullptr, &_sampler) != VK_SUCCESS) {
		fmt::print("Texture: failed to create sampler for: {}", _filePath.string());
		return false;
	}

	return true;
}

bool Texture::doUnload(VulkanCore* core)
{
	if (_sampler != VK_NULL_HANDLE) {
		vkDestroySampler(core->get_device(), _sampler, nullptr);
		_sampler = VK_NULL_HANDLE;
	}
	if (_image != VK_NULL_HANDLE) {
		AllocatedImage allocated{ _image, _imageView, _allocation };
		core->destroy_image(allocated);

		_image = VK_NULL_HANDLE;
		_imageView = VK_NULL_HANDLE;
		_allocation = VK_NULL_HANDLE;
	}

	return true;
}
