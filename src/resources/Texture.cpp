#include "resources/Texture.h"
#include <resources/CookedFormats.h>
#include <resources/AssetImporter.h>
#include <resources/Buffer.h>
#include <graphics/VulkanCore.h>
#include <graphics/utils/StagingUploader.h>
#include <graphics/utils/vk_utils.h>
#include <fmt/core.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <fstream>
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
	stbi_uc* stbiPixels = nullptr;
	std::vector<uint8_t> cookedPixels;
	const uint8_t* uploadPixels = nullptr;
	size_t uploadByteSize = 0;
	bool loadedFromCookedGtex = false;
	CookedTextureHeader gtexHeader{};

	// check if cooked .gtex binary exists for this texture
	if (_sourceType == SourceType::File) {
		std::filesystem::path gtexPath;
		if (_filePath.extension() == ".gtex") {
			gtexPath = _filePath;
		}
		else if (const AssetMetadata* meta = AssetRegistry::get().get_metadata(get_uuid())) {
			if (!meta->cookedPath.empty()) {
				gtexPath = meta->cookedPath; 

				if (!std::filesystem::exists(gtexPath) && std::filesystem::exists(meta->virtualPath) && !meta->is_sub_asset()) {
					AssetImporter::cook_texture(*meta);
				}
			}
		}

		if (!gtexPath.empty() && std::filesystem::exists(gtexPath)) {
			std::ifstream in(gtexPath, std::ios::binary);
			if (in.is_open()) {
				in.read(reinterpret_cast<char*>(&gtexHeader), sizeof(CookedTextureHeader));
				if (in.good() && gtexHeader.magic == GTEX_MAGIC && gtexHeader.payloadSizeBytes > 0) {
					cookedPixels.resize(gtexHeader.payloadSizeBytes);
					in.read(reinterpret_cast<char*>(cookedPixels.data()), static_cast<std::streamsize>(gtexHeader.payloadSizeBytes));

					if (in.good()) {
						loadedFromCookedGtex = true;
						width = static_cast<int>(gtexHeader.width);
						height = static_cast<int>(gtexHeader.height);
						_extent = { gtexHeader.width, gtexHeader.height, gtexHeader.depth };
						_mipLevels = gtexHeader.mipLevels;
						_format = gtexHeader.format;
						_mipmapped = (_mipLevels > 1);
						uploadPixels = cookedPixels.data();
						uploadByteSize = cookedPixels.size();
					}
				}
			}
		}

		// fallback to raw stb_image file if .gtex not found
		if (!loadedFromCookedGtex) {
			if (_filePath.extension() == ".gtex") {
				fmt::println("Texture: cooked .gtex file missing or corrupt: {}", _filePath.string());
				return false;
			}
			stbiPixels = stbi_load(_filePath.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
			if (!stbiPixels) {
				fmt::print("Texture: failed to load pixels from file: {}\n", _filePath.string());
				return false;
			}
			uploadPixels = stbiPixels;
			uploadByteSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
		}
	}
	else if (_sourceType == SourceType::Encoded) {
		stbiPixels = stbi_load_from_memory(_bufferData.data(), static_cast<int>(_bufferData.size()), &width, &height, &channels, STBI_rgb_alpha);
		_bufferData.clear();
		if (!stbiPixels) {
			fmt::print("Texture: failed to load pixels from memory\n");
			return false;
		}
	}
	else if (_sourceType == SourceType::Description) {
		width = _extent.width;
		height = _extent.height;
		if (!_bufferData.empty()) {
			uploadPixels = _bufferData.data();
			uploadByteSize = _bufferData.size();
		}
	}

	if (_sourceType != SourceType::Description && !loadedFromCookedGtex) {
		_extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
		_mipLevels = _mipmapped ? static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1u : 1u;
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
	if (uploadPixels) {
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

	// upload pixel data via staginguploader
	if (uploadPixels && uploadByteSize > 0) {
		StagedTextureUploadDesc desc{
			.dstImage = _image,
			.format = _format,
			.extent = _extent,
			.mipLevels = _mipLevels,
			.arrayLayers = imgInfo.arrayLayers,
			.aspectMask = viewInfo.subresourceRange.aspectMask,
			.generateMips = (_mipmapped && _mipLevels > 1)
		};

		UUID selfUuid = get_uuid();
		_resourceManager->get_staging_uploader().stage_texture_upload(
			desc,
			uploadPixels,
			uploadByteSize,
			[selfUuid, uploadByteSize]() { // callback to mark texture as loaded
				if (selfUuid.is_valid()) {
					AssetRegistry::get().set_resource_state(selfUuid, ResourceState::Loaded);
					AssetRegistry::get().set_gpu_size(selfUuid, uploadByteSize);
				}
			}
		);

		AssetRegistry::get().set_gpu_size(selfUuid, uploadByteSize);

		if (stbiPixels) {
			stbi_image_free(stbiPixels);
		}
		_bufferData.clear();
	}
	else if (_sourceType == SourceType::Description && (_usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT || _usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
		// leave undefined; RenderGraph manages runtime transitions
	}
	else if (_sourceType == SourceType::Description && !_desc.deferBindlessRegistration) {
		_core->immediate_submit([&](VkCommandBuffer cmd) {
			vkutil::transition_image(cmd, _image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
			});
	}

	VkSamplerCreateInfo samplerInfo = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	if (loadedFromCookedGtex) { // create sampler using .gtex settings
		samplerInfo.magFilter = gtexHeader.magFilter;
		samplerInfo.minFilter = gtexHeader.minFilter;
		samplerInfo.mipmapMode = gtexHeader.mipmapMode;
		samplerInfo.addressModeU = gtexHeader.addressModeU;
		samplerInfo.addressModeV = gtexHeader.addressModeV;
		samplerInfo.addressModeW = gtexHeader.addressModeU;
	}
	else if (_sourceType == SourceType::Description) { // create sampler using description settings
		samplerInfo.magFilter = _desc.magFilter;
		samplerInfo.minFilter = _desc.minFilter;
		samplerInfo.mipmapMode = _desc.mipmapMode;
		samplerInfo.addressModeU = _desc.addressModeU;
		samplerInfo.addressModeV = _desc.addressModeV;
		samplerInfo.addressModeW = _desc.addressModeW;
	}
	else { // create samplers with lienar defaults
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	}
	samplerInfo.minLod = 0.0f;
	samplerInfo.maxLod = static_cast<float>(_mipLevels);
	samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
	samplerInfo.unnormalizedCoordinates = VK_FALSE;

	if (vkCreateSampler(_core->get_device(), &samplerInfo, nullptr, &_sampler) != VK_SUCCESS) {
		fmt::println("Texture: failed to create sampler for: {}", get_id());
		return false;
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
