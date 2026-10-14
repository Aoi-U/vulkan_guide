#include "resources/Texture.h"
#include <graphics/VulkanCore.h>
#include <resources/Buffer.h>
#include <graphics/utils/vk_utils.h>
#include <fmt/core.h>
#define STB_IMAGE_IMPLEMENTATION 
#include <stb_image.h>
#include <cmath>
#include <algorithm>

Texture::Texture(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, const std::filesystem::path& filepath, VkFormat format, VkImageUsageFlags usage, bool generateMips)
	: Resource(id, core, resourceManager), _sourceType(SourceType::File), _filePath(filepath), _format(format), _usage(usage), _mipmapped(generateMips) {
	if (_filePath.empty()) {
		_filePath = std::filesystem::path("assets/textures") / (id + ".png");
	}
}

Texture::Texture(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, std::span<const uint8_t> data, VkFormat format, VkImageUsageFlags usage, bool generateMips)
	: Resource(id, core, resourceManager), _sourceType(SourceType::Encoded), _bufferData(data.begin(), data.end()), _format(format), _usage(usage), _mipmapped(generateMips) {}

Texture::Texture(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, const TextureDescription& desc, std::span<const uint8_t> data)
	: Resource(id, core, resourceManager), _sourceType(SourceType::Description), _bufferData(data.begin(), data.end()), _desc(desc), _format(desc.format), _usage(desc.usage), _mipmapped(desc.mipLevels > 1), _extent(desc.extent), _mipLevels(desc.mipLevels) {}

void Texture::destroy_immediate()
{
	if (_image != VK_NULL_HANDLE) {
		vmaDestroyImage(_core->get_allocator(), _image, _allocation);
		_image = VK_NULL_HANDLE;
		_allocation = VK_NULL_HANDLE;
	}
	if (_imageView != VK_NULL_HANDLE) {
		vkDestroyImageView(_core->get_device(), _imageView, nullptr);
		_imageView = VK_NULL_HANDLE;
	}
	if (_sampler != VK_NULL_HANDLE) {
		vkDestroySampler(_core->get_device(), _sampler, nullptr);
		_sampler = VK_NULL_HANDLE;
	}
	for (VkImageView view : _faceViews) {
		if (view != VK_NULL_HANDLE) {
			vkDestroyImageView(_core->get_device(), view, nullptr);
		}
	}
	_faceViews.clear();
}

bool Texture::doLoad()
{
	int width = 0, height = 0, channels = 0;
	stbi_uc* pixels = nullptr;
	bool isRaw = false;

	// load pixels based on source type
	if (_sourceType == SourceType::File) {
		pixels = stbi_load(_filePath.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
		if (!pixels) {
			fmt::print("Texture: failed to load pixels from file: {}\n", _filePath.string());
			return false;
		}
	}
	else if (_sourceType == SourceType::Encoded) {
		pixels = stbi_load_from_memory(_bufferData.data(), static_cast<int>(_bufferData.size()), &width, &height, &channels, STBI_rgb_alpha);
		_bufferData.clear();
		if (!pixels) {
			fmt::print("Texture: failed to load pixels from memory\n");
			return false;
		}
	}
	else if (_sourceType == SourceType::Description) {
		width = _extent.width;
		height = _extent.height;
		if (!_bufferData.empty()) {
			pixels = _bufferData.data();
			isRaw = true;
		}
	}

	if (_sourceType != SourceType::Description) {
		_extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
		_mipLevels = _mipmapped ? static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1 : 1;
	}

	// setup image create info
	VkImageCreateInfo imgInfo{ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	imgInfo.imageType = _extent.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
	imgInfo.format = _format;
	imgInfo.extent = _extent;
	imgInfo.mipLevels = _mipLevels;
	imgInfo.arrayLayers = (_sourceType == SourceType::Description) ? _desc.arrayLayers : 1;
	imgInfo.samples = (_sourceType == SourceType::Description) ? _desc.samples : VK_SAMPLE_COUNT_1_BIT;
	imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imgInfo.usage = _usage;
	if (pixels) {
		imgInfo.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	}
	if (_sourceType == SourceType::Description) {
		imgInfo.flags = _desc.flags;
	}

	// create the image and allocate memory for it
	VmaAllocationCreateInfo allocInfo{ .usage = VMA_MEMORY_USAGE_GPU_ONLY, .requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	if (vmaCreateImage(_core->get_allocator(), &imgInfo, &allocInfo, &_image, &_allocation, nullptr) != VK_SUCCESS) {
		fmt::print("Texture: failed to create vulkan image for: {}\n", get_id());
		return false;
	}

	// setup image view create info
	VkImageViewCreateInfo viewInfo{ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	viewInfo.viewType = (_sourceType == SourceType::Description) ? _desc.viewType : VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.image = _image;
	viewInfo.format = _format;
	viewInfo.subresourceRange.baseMipLevel = 0;
	viewInfo.subresourceRange.levelCount = _mipLevels;
	viewInfo.subresourceRange.baseArrayLayer = 0;
	viewInfo.subresourceRange.layerCount = imgInfo.arrayLayers;
	viewInfo.subresourceRange.aspectMask = (_sourceType == SourceType::Description) ? _desc.aspectMask : VK_IMAGE_ASPECT_COLOR_BIT;

	// create the image view
	if (vkCreateImageView(_core->get_device(), &viewInfo, nullptr, &_imageView) != VK_SUCCESS) {
		fmt::print("Texture: failed to create image view for: {}\n", get_id());
		return false;
	}

	if (imgInfo.arrayLayers > 1) {
		_faceViews.resize(imgInfo.arrayLayers, VK_NULL_HANDLE);
		for (uint32_t i = 0; i < imgInfo.arrayLayers; i++) {
			VkImageViewCreateInfo faceInfo = viewInfo;
			faceInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			faceInfo.subresourceRange.baseArrayLayer = i;
			faceInfo.subresourceRange.layerCount = 1;
			vkCreateImageView(_core->get_device(), &faceInfo, nullptr, &_faceViews[i]);
		}

		if (imgInfo.arrayLayers > 1 && (_usage & VK_IMAGE_USAGE_STORAGE_BIT)) {
			_mipArrayViews.resize(_mipLevels, VK_NULL_HANDLE);
			for (uint32_t mip = 0; mip < _mipLevels; mip++) {
				VkImageViewCreateInfo mipArrayInfo = viewInfo;
				mipArrayInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
				mipArrayInfo.subresourceRange.baseMipLevel = mip;
				mipArrayInfo.subresourceRange.levelCount = 1;
				mipArrayInfo.subresourceRange.baseArrayLayer = 0;
				mipArrayInfo.subresourceRange.layerCount = imgInfo.arrayLayers;
				vkCreateImageView(_core->get_device(), &mipArrayInfo, nullptr, &_mipArrayViews[mip]);
			}
		}
	}

	if (pixels) { // if there is pixel data, upload to GPU
		size_t dataSize = width * height * 4 * _extent.depth;
		if (isRaw)
			dataSize = _bufferData.size();

		//Buffer staging(get_id() + "_staging", _core, dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
		//staging.load();
		//staging.copy_data(pixels, dataSize);
		ResourceHandle<Buffer> staging = _resourceManager->load<Buffer>(get_id() + "_staging", std::span<const uint8_t>(pixels, dataSize), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
		staging->copy_data(pixels, dataSize);

		_core->immediate_submit([&](VkCommandBuffer cmd) {
			vkutil::image_mem_barrier2(cmd, _image, _format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, imgInfo.arrayLayers, _mipLevels, 0);

			VkBufferImageCopy copyRegion = {};
			copyRegion.imageSubresource.aspectMask = viewInfo.subresourceRange.aspectMask;
			copyRegion.imageSubresource.layerCount = imgInfo.arrayLayers;
			copyRegion.imageExtent = _extent;
			vkCmdCopyBufferToImage(cmd, staging->get_buffer(), _image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

			if (_mipmapped && _mipLevels > 1) {
				vkutil::generate_mipmaps(cmd, _image, { _extent.width, _extent.height });
			}
			else {
				vkutil::image_mem_barrier2(cmd, _image, _format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, imgInfo.arrayLayers, _mipLevels, 0);
			}
			});
		//staging.destroy_immediate();

		if (!isRaw) {
			stbi_image_free(pixels);
		}
		if (isRaw) {
			_bufferData.clear();
		}
	}
	else if (_sourceType == SourceType::Description && (_usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT || _usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
		// leave undefined; RenderGraph manages runtime transitions
	}
	else if (_sourceType == SourceType::Description && !_desc.deferBindlessRegistration) {
		_core->immediate_submit([&](VkCommandBuffer cmd) {
			vkutil::transition_image(cmd, _image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
			});
	}

	// create sampler if not description type
	if (_sourceType != SourceType::Description) {
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

		if (vkCreateSampler(_core->get_device(), &samplerInfo, nullptr, &_sampler) != VK_SUCCESS) {
			fmt::print("Texture: failed to create sampler for: {}\n", get_id());
			return false;
		}
	}
	else {
		VkSamplerCreateInfo samplerInfo = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
		samplerInfo.magFilter = _desc.magFilter;
		samplerInfo.minFilter = _desc.minFilter;
		samplerInfo.mipmapMode = _desc.mipmapMode;
		samplerInfo.addressModeU = _desc.addressModeU;
		samplerInfo.addressModeV = _desc.addressModeV;
		samplerInfo.addressModeW = _desc.addressModeW;
		samplerInfo.minLod = 0.0f;
		samplerInfo.maxLod = static_cast<float>(_mipLevels);
		samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
		samplerInfo.unnormalizedCoordinates = VK_FALSE;

		if (vkCreateSampler(_core->get_device(), &samplerInfo, nullptr, &_sampler) != VK_SUCCESS) {
			fmt::print("Texture: failed to create sampler for: {}\n", get_id());
			return false;
		}
	}

	// register texture in bindless descriptor set
	if ((_sourceType != SourceType::Description || (_usage & VK_IMAGE_USAGE_SAMPLED_BIT)) && !_desc.deferBindlessRegistration) { // only register if it's a sampled texture
		_bindlessIndex = _resourceManager->allocate_bindless_texture(_imageView, _sampler);
	}

	return true;
}

bool Texture::doUnload()
{
	if (_bindlessIndex != 0) {
		_core->get_deletion_queue().push_function([rm = _resourceManager, index = _bindlessIndex]() {
			if (rm) {
				rm->free_bindless_slot(index);
			}
			});
		_bindlessIndex = 0;
	}

	if (_sampler != VK_NULL_HANDLE) {
		_core->get_deletion_queue().push(_sampler);
		_sampler = VK_NULL_HANDLE;
	}

	for (VkImageView view : _faceViews) {
		if (view != VK_NULL_HANDLE) {
			_core->get_deletion_queue().push(view);
		}
	}
	_faceViews.clear();

	for (VkImageView view : _mipArrayViews) {
		if (view != VK_NULL_HANDLE) {
			_core->get_deletion_queue().push(view);
		}
	}
	_mipArrayViews.clear();

	if (_imageView != VK_NULL_HANDLE) {
		_core->get_deletion_queue().push(_imageView);
		_imageView = VK_NULL_HANDLE;
	}

	if (_image != VK_NULL_HANDLE) {
		_core->get_deletion_queue().push(_image, _allocation);
		_image = VK_NULL_HANDLE;
		_allocation = VK_NULL_HANDLE;
	}

	return true;
}
