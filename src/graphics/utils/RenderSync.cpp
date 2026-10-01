#include "RenderSync.h"

RenderSync::SyncAccessInfo RenderSync::get_sync_info(ResourceState state)
{
	switch (state)
	{
	case ResourceState::Undefined:
		return { VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE };
	case ResourceState::ColorAttachment:
		return {
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT
		};
	case ResourceState::DepthAttachment:
		return {
			VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
			VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
			VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT
		};
	case ResourceState::DepthReadOnly:
		return {
			VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			VK_ACCESS_2_SHADER_READ_BIT
		};
	case ResourceState::ShaderReadOnly:
		return {
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			VK_ACCESS_2_SHADER_READ_BIT
		};
	case ResourceState::StorageImage:
		return {
			VK_IMAGE_LAYOUT_GENERAL,
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
		};
	case ResourceState::TransferSrc:
		return {
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
				VK_ACCESS_2_TRANSFER_READ_BIT
		};

	case ResourceState::TransferDst:
		return {
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
				VK_ACCESS_2_TRANSFER_WRITE_BIT
		};

	case ResourceState::Present:
		return {
				VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
				VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
				VK_ACCESS_2_NONE
		};
	}

	return { VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE };
}

void RenderSync::transition_image(VkCommandBuffer cmd, VkImage image, ResourceState oldState, ResourceState newState, VkImageAspectFlags aspectMask, uint32_t baseMipLevel, uint32_t levelCount, uint32_t baseArrayLayer, uint32_t layerCount)
{
	// create a single barrier info 
	ImageBarrierInfo barrierInfo{
		.image = image,
		.oldState = oldState,
		.newState = newState,
		.aspectMask = aspectMask,
		.baseMipLevel = baseMipLevel,
		.levelCount = levelCount,
		.baseArrayLayer = baseArrayLayer,
		.layerCount = layerCount
	};

	transition_images(cmd, { &barrierInfo , 1});
}

void RenderSync::transition_images(VkCommandBuffer cmd, const std::span<ImageBarrierInfo>& barriers)
{
	if (barriers.empty())
		return;

	std::vector<VkImageMemoryBarrier2> imageBarriers;
	imageBarriers.reserve(barriers.size());

	// create a barrier for each image
	for (const auto& b : barriers) {
		auto src = get_sync_info(b.oldState);
		auto dst = get_sync_info(b.newState);

		VkImageMemoryBarrier2 barrier{ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
		barrier.srcStageMask = src.stageMask; 
		barrier.srcAccessMask = src.accessMask;
		barrier.dstStageMask = dst.stageMask;
		barrier.dstAccessMask = dst.accessMask;
		barrier.oldLayout = src.layout;
		barrier.newLayout = dst.layout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = b.image;
		barrier.subresourceRange.aspectMask = b.aspectMask;
		barrier.subresourceRange.baseMipLevel = b.baseMipLevel;
		barrier.subresourceRange.levelCount = b.levelCount;
		barrier.subresourceRange.baseArrayLayer = b.baseArrayLayer;
		barrier.subresourceRange.layerCount = b.layerCount;

		imageBarriers.push_back(barrier);
	}

	VkDependencyInfo depInfo{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	depInfo.imageMemoryBarrierCount = static_cast<uint32_t>(imageBarriers.size());
	depInfo.pImageMemoryBarriers = imageBarriers.data();

	vkCmdPipelineBarrier2(cmd, &depInfo);
}
