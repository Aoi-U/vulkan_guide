#include "RenderTargetPool.h"
#include <fmt/core.h>
#include <graphics/utils/vk_utils.h>

void RenderTargetPool::init(VkDevice device, VmaAllocator allocator)
{
	_device = device;
	_allocator = allocator;
}

void RenderTargetPool::cleanup()
{
	for (auto& [key, pooledImages] : _pool) {
		for (auto& pooledImage : pooledImages) {
			vkDestroyImageView(_device, pooledImage.imageView, nullptr); // destroy image view
			for (auto faceView : pooledImage.faceViews) { // destroy face views
				if (faceView != VK_NULL_HANDLE) {
					vkDestroyImageView(_device, faceView, nullptr);
				}
			}
			vmaDestroyImage(_allocator, pooledImage.image, pooledImage.allocation); // destroy image
		}
	}
	_pool.clear();
}

void RenderTargetPool::reset_frame(uint32_t currentFrame)
{
	for (auto it = _pool.begin(); it != _pool.end(); ) {
		auto& pooledImages = it->second;

		for (auto imgIt = pooledImages.begin(); imgIt != pooledImages.end(); ) {
			// destroy unused images that have not been used for 4 frames (arbitrary number)
			if (!imgIt->inUse && (currentFrame > imgIt->lastUsedFrame + 4)) {
				if (imgIt->imageView != VK_NULL_HANDLE) {
					vkDestroyImageView(_device, imgIt->imageView, nullptr);
				}
				for (auto face : imgIt->faceViews) {
					if (face != VK_NULL_HANDLE) {
						vkDestroyImageView(_device, face, nullptr);
					}
				}
				if (imgIt->image != VK_NULL_HANDLE) {
					vmaDestroyImage(_allocator, imgIt->image, imgIt->allocation);
				}
				imgIt = pooledImages.erase(imgIt);
			}
			else { 
				imgIt->inUse = false;
				imgIt++;
			}
		}

		if (pooledImages.empty()) {
			it = _pool.erase(it);
		}
		else {
			++it;
		}
	}
}

PooledImage* RenderTargetPool::get_image(const AllocationKey& key, uint32_t currentFrame)
{
	auto& pooledImages = _pool[key];

	// look for an unused image in the pool that matches the key
	for (auto& pooledImage : pooledImages) {
		if (!pooledImage.inUse) {
			pooledImage.inUse = true;
			pooledImage.lastUsedFrame = currentFrame;
			return &pooledImage;
		}
	}

	// create new image 
	PooledImage newImage;
	newImage.inUse = true;
	newImage.lastUsedFrame = currentFrame;

	VkImageCreateInfo imgInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.flags = key.flags,
		.imageType = key.extent.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
		.format = key.format,
		.extent = key.extent,
		.mipLevels = key.mipLevels,
		.arrayLayers = key.arrayLayers,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = key.usage,
	};

	VmaAllocationCreateInfo allocInfo{ .usage = VMA_MEMORY_USAGE_GPU_ONLY };
	if (vmaCreateImage(_allocator, &imgInfo, &allocInfo, &newImage.image, &newImage.allocation, nullptr) != VK_SUCCESS) {
		fmt::println("RenderTargetPool: failed to create new target image");
		return nullptr;
	}

	VkImageViewCreateInfo viewInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = newImage.image,
		.viewType = key.viewType,
		.format = key.format
	};
	viewInfo.subresourceRange.baseMipLevel = 0;
	viewInfo.subresourceRange.levelCount = key.mipLevels;
	viewInfo.subresourceRange.baseArrayLayer = 0;
	viewInfo.subresourceRange.layerCount = key.arrayLayers;
	viewInfo.subresourceRange.aspectMask = (key.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

	if (vkutil::has_stencil_component(key.format)) {
		viewInfo.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
	}

	vkCreateImageView(_device, &viewInfo, nullptr, &newImage.imageView);

	if (key.arrayLayers > 1) {
		newImage.faceViews.resize(key.arrayLayers, VK_NULL_HANDLE);
		for (uint32_t i = 0; i < key.arrayLayers; i++) {
			VkImageViewCreateInfo faceInfo = viewInfo;
			faceInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			faceInfo.subresourceRange.baseArrayLayer = i;
			faceInfo.subresourceRange.layerCount = 1;

			vkCreateImageView(_device, &faceInfo, nullptr, &newImage.faceViews[i]);
		}
	}

	pooledImages.push_back(newImage);
	return &pooledImages.back();
}
