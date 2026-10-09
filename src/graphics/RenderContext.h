#pragma once

#include <graphics/utils/vk_types.h>
#include <graphics/Pipeline.h>
#include <span>
#include <optional>

struct ColorAttachmentDesc
{
	VkImageView view;
	VkAttachmentLoadOp loadOp{ VK_ATTACHMENT_LOAD_OP_LOAD };
	VkAttachmentStoreOp storeOp{ VK_ATTACHMENT_STORE_OP_STORE };
	VkClearColorValue clearColor{ 0.0f, 0.0f, 0.0f, 1.0f };
	VkImageLayout layout{ VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
};

struct DepthAttachmentDesc
{
	VkImageView view;
	VkAttachmentLoadOp loadOp{ VK_ATTACHMENT_LOAD_OP_LOAD };
	VkAttachmentStoreOp storeOp{ VK_ATTACHMENT_STORE_OP_STORE };
	float clearDepth{ 0.0f };
	VkImageLayout layout{ VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL };
};

struct RenderDesc
{
	VkExtent2D renderExtent{};
	std::vector<ColorAttachmentDesc> colorAttachments;
	std::optional<DepthAttachmentDesc> depthAttachment;
};

// context for managing render passes
class RenderContext
{
public:
	explicit RenderContext(VkCommandBuffer cmd) : _cmd(cmd) {}

	VkCommandBuffer get_cmd() const { return _cmd; }

	void begin_rendering(const RenderDesc& desc);
	void end_rendering();

	void set_viewport_scissor(const VkExtent2D extent);
	void set_cull_mode(VkCullModeFlags cullMode);
	void set_front_face(VkFrontFace frontFace);

	void bind_pipeline(const GraphicsPipeline& pipeline);
	void bind_pipeline(const ComputePipeline& pipeline);

	void bind_descriptor_set(VkPipelineBindPoint bindPoint, VkPipelineLayout layout, uint32_t setIndex, VkDescriptorSet set); // bind a single descriptor set
	void bind_descriptor_sets(VkPipelineBindPoint bindPoint, VkPipelineLayout layout, uint32_t firstSet, std::span<const VkDescriptorSet> sets); // bind multiple descriptor sets

	void bind_index_buffer(VkBuffer buffer, VkDeviceSize offset = 0, VkIndexType indexType = VK_INDEX_TYPE_UINT32);

	// draw commands
	void draw(uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0);
	void draw_indexed_indirect_count(VkBuffer cmdBuffer, VkDeviceSize offset, VkBuffer countBufferOffset, VkDeviceSize countOffset, uint32_t maxDrawCount, uint32_t stride);
	void dispatch(uint32_t groupCountX, uint32_t groupCountY = 1, uint32_t groupCountZ = 1);

	// set arbitary push constants
	template<typename T>
	void set_push_constants(VkPipelineLayout layout, VkShaderStageFlags stages, const T& data, uint32_t offset = 0) {
		vkCmdPushConstants(_cmd, layout, stages, offset, sizeof(T), &data);
	}

private:
	VkCommandBuffer _cmd;
};