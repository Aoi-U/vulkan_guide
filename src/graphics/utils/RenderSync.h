#pragma once

#include <core/vk_types.h>
#include <span>

// abstraction layer for vulkan dependency and barriers to manage resource state transitions

enum class ResourceState
{
	Undefined,
	ColorAttachment,
	DepthAttachment,
	DepthReadOnly,
	ShaderReadOnly,
	StorageImage,
	TransferSrc,
	TransferDst,
	Present
};

struct ImageBarrierInfo
{
	VkImage image;
	ResourceState oldState{ ResourceState::Undefined };
	ResourceState newState{ ResourceState::Undefined };
	VkImageAspectFlags aspectMask{ VK_IMAGE_ASPECT_COLOR_BIT };
	uint32_t baseMipLevel{ 0 };
	uint32_t levelCount{ VK_REMAINING_MIP_LEVELS };
	uint32_t baseArrayLayer{ 0 };
	uint32_t layerCount{ VK_REMAINING_ARRAY_LAYERS };
};

namespace RenderSync
{
	struct SyncAccessInfo
	{
		VkImageLayout layout;
		VkPipelineStageFlags2 stageMask;
		VkAccessFlags2 accessMask;
	};

	SyncAccessInfo get_sync_info(ResourceState state);

	void transition_image(VkCommandBuffer cmd, VkImage image, ResourceState oldState, ResourceState newState, VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, 
		uint32_t baseMipLevel = 0, uint32_t levelCount = VK_REMAINING_MIP_LEVELS, uint32_t baseArrayLayer = 0, uint32_t layerCount = VK_REMAINING_ARRAY_LAYERS);

	void transition_images(VkCommandBuffer cmd, const std::span<ImageBarrierInfo>& barriers);
}