
#pragma once 

#include <vulkan/vulkan.h>

namespace vkutil 
{
	void transition_image(VkCommandBuffer cmd, VkImage image, VkImageLayout currentLayout, VkImageLayout newLayout);

	void copy_image_to_image(VkCommandBuffer cmd, VkImage source, VkImage destination, VkExtent2D srcSize, VkExtent2D dstSize);

	void generate_mipmaps(VkCommandBuffer cmd, VkImage image, VkExtent2D imageSize);

	bool has_stencil_component(VkFormat format);

	void image_mem_barrier2(VkCommandBuffer CmdBuf, VkImage Image, VkFormat Format, VkImageLayout OldLayout, VkImageLayout NewLayout,
		uint32_t LayerCount = 1, uint32_t MipLevels = 1, uint32_t BaseMipLevel = 0);

	void buffer_mem_barrier2(VkCommandBuffer CmdBuf, VkBuffer Buffer, VkPipelineStageFlags2 SrcStage, VkPipelineStageFlags2 DstStage);

};