#include "graphics/RenderContext.h"
#include <utils/vk_initializers.h>

void RenderContext::begin_rendering(const RenderDesc& desc)
{
	// setup color attachment infos
	std::vector<VkRenderingAttachmentInfo> colorInfos;
	colorInfos.reserve(desc.colorAttachments.size());

	for (const ColorAttachmentDesc& col : desc.colorAttachments) {
		// create color attachment info
		VkRenderingAttachmentInfo info{ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		info.imageView = col.view;
		info.imageLayout = col.layout;
		info.loadOp = col.loadOp;
		info.storeOp = col.storeOp;
		info.clearValue.color = col.clearColor;

		colorInfos.push_back(info);
	}

	// setup depth attachment info
	VkRenderingAttachmentInfo depthInfo{ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	bool hasDepth = desc.depthAttachment.has_value();
	if (hasDepth) {
		depthInfo.imageView = desc.depthAttachment->view;
		depthInfo.imageLayout = desc.depthAttachment->layout;
		depthInfo.loadOp = desc.depthAttachment->loadOp;
		depthInfo.storeOp = desc.depthAttachment->storeOp;
		depthInfo.clearValue.depthStencil.depth = desc.depthAttachment->clearDepth;
	}

	// setup rendering info
	VkRenderingInfo renderInfo{ .sType = VK_STRUCTURE_TYPE_RENDERING_INFO };
	renderInfo.renderArea = VkRect2D{ {0, 0}, desc.renderExtent };
	renderInfo.layerCount = 1;
	renderInfo.colorAttachmentCount = static_cast<uint32_t>(colorInfos.size());
	renderInfo.pColorAttachments = colorInfos.data();
	renderInfo.pDepthAttachment = hasDepth ? &depthInfo : nullptr;

	// begin rendering
	vkCmdBeginRendering(_cmd, &renderInfo);
}

void RenderContext::end_rendering()
{
	vkCmdEndRendering(_cmd);
}

void RenderContext::set_viewport_scissor(const VkExtent2D extent)
{
	// set viewport
	VkViewport viewport{ 0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
	vkCmdSetViewport(_cmd, 0, 1, &viewport);

	// set scissor
	VkRect2D scissor{ {0, 0}, extent };
	vkCmdSetScissor(_cmd, 0, 1, &scissor);
}

void RenderContext::set_cull_mode(VkCullModeFlags cullMode, VkFrontFace frontFace)
{
	vkCmdSetCullMode(_cmd, cullMode);
	vkCmdSetFrontFace(_cmd, frontFace);
}

void RenderContext::bind_pipeline(const GraphicsPipeline& pipeline)
{
	vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.get_pipeline());
}

void RenderContext::bind_pipeline(const ComputePipeline & pipeline)
{
	vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.get_pipeline());
}

void RenderContext::bind_descriptor_set(VkPipelineBindPoint bindPoint, VkPipelineLayout layout, uint32_t setIndex, VkDescriptorSet set)
{
	vkCmdBindDescriptorSets(_cmd, bindPoint, layout, setIndex, 1, &set, 0, nullptr);
}

void RenderContext::bind_descriptor_sets(VkPipelineBindPoint bindPoint, VkPipelineLayout layout, uint32_t firstSet, std::span<const VkDescriptorSet> sets)
{
	vkCmdBindDescriptorSets(_cmd, bindPoint, layout, firstSet, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
}

void RenderContext::bind_index_buffer(VkBuffer buffer, VkDeviceSize offset, VkIndexType indexType)
{
	vkCmdBindIndexBuffer(_cmd, buffer, offset, indexType);
}

void RenderContext::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
	vkCmdDraw(_cmd, vertexCount, instanceCount, firstVertex, firstInstance);
}

void RenderContext::draw_indexed_indirect_count(VkBuffer cmdBuffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride)
{
	vkCmdDrawIndexedIndirectCount(_cmd, cmdBuffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

void RenderContext::dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
	vkCmdDispatch(_cmd, groupCountX, groupCountY, groupCountZ);
}


