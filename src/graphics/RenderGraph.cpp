#include "RenderGraph.h"
#include <graphics/utils/vk_utils.h>

RGPassBuilder& RGPassBuilder::read_texture(const std::string& name, VkImageLayout layout)
{
	_pass.textureTransitions.push_back({ name, layout });
	return* this;
}

RGPassBuilder& RGPassBuilder::write_texture(const std::string& name, VkImageLayout layout)
{
	_pass.textureTransitions.push_back({ name, layout });
	return* this;
}

RGPassBuilder& RGPassBuilder::read_buffer(const std::string& name, VkPipelineStageFlags2 stage)
{
	_pass.bufferTransitions.push_back({ name, stage });
	return *this;
}

RGPassBuilder& RGPassBuilder::write_buffer(const std::string& name, VkPipelineStageFlags2 stage)
{ 
	_pass.bufferTransitions.push_back({ name, stage });
	return *this;
}

RGPassBuilder& RGPassBuilder::set_color_attachment(const std::string& name, VkImageView view, VkAttachmentLoadOp loadOp, const VkClearColorValue& clearColor)
{
	_pass.textureTransitions.push_back({ name, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
	_pass.colorAttachments.push_back({ name, view, loadOp, VK_ATTACHMENT_STORE_OP_STORE, clearColor });
	return *this;
}

RGPassBuilder& RGPassBuilder::set_depth_attachment(const std::string& name, VkImageView view, VkAttachmentLoadOp loadOp, float clearDepth)
{
	_pass.textureTransitions.push_back({ name, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL });
	_pass.depthAttachment = RGDepthAttachment{ name, view, loadOp, VK_ATTACHMENT_STORE_OP_STORE, clearDepth };
	return *this;
}

RGPassBuilder& RGPassBuilder::set_extent(VkExtent2D extent)
{
	_pass.renderExtent = extent;
	return *this;
}

RGPassBuilder& RGPassBuilder::set_execute_callback(ExecutionFn function)
{
	_pass.executeCallback = std::move(function);
	return *this;
}

void RenderGraph::clear_passes()
{
	_passes.clear();
}

void RenderGraph::register_texture(const std::string & name, VkImage image, VkFormat format, VkImageLayout initialLayout, uint32_t layerCount, uint32_t mipLevels, uint32_t baseMiplevel)
{
	_textures[name] = RGTextureState{ image, format, initialLayout, layerCount, mipLevels, baseMiplevel };
}

void RenderGraph::register_buffer(const std::string & name, VkBuffer buffer, VkPipelineStageFlags2 initialStage)
{
	_buffers[name] = RGBufferState{ buffer, initialStage };
}

RGPassBuilder RenderGraph::add_pass(const std::string name)
{
	_passes.push_back(RGPass{ .name = name });
	return RGPassBuilder(*this, _passes.back());
}

void RenderGraph::execute(RenderContext& ctx)
{
	VkCommandBuffer cmd = ctx.get_cmd();

	for (RGPass& pass : _passes) {
		// dispatch texture barriers
		for (const RGTextureTransition& transition : pass.textureTransitions) {
			RGTextureState* tex = get_texture(transition.name);
			if (!tex) {
				fmt::println("RenderGraph: texture {} not registered", transition.name);
				continue;
			}

			if (tex->currentLayout == transition.targetLayout) {
				continue; // no transition needed, already in correct layout
			}

			vkutil::image_mem_barrier2(cmd, tex->image, tex->format, tex->currentLayout, transition.targetLayout, tex->layerCount, tex->mipLevels, tex->baseMiplevel);
			tex->currentLayout = transition.targetLayout;
		}

		// dispatch buffer barriers
		for (const RGBufferTransition& transition : pass.bufferTransitions) {
			RGBufferState* buf = get_buffer(transition.name);
			if (!buf) {
				fmt::println("RenderGraph: buffer {} not registered", transition.name);
				continue;
			}

			if (buf->currentStage == transition.targetStage) {
				continue; // no transition needed, already in correct stage
			}

			vkutil::buffer_mem_barrier2(cmd, buf->buffer, buf->currentStage, transition.targetStage);
			buf->currentStage = transition.targetStage; 
		}

		// execute the pass callback
		bool hasColor = !pass.colorAttachments.empty();
		bool hasDepth = pass.depthAttachment.has_value();

		if (hasColor || hasDepth) {
			RenderDesc renderDesc;
			renderDesc.renderExtent = pass.renderExtent.value_or(VkExtent2D{ 0, 0 });
			
			for (const RGColorAttachment& color : pass.colorAttachments) {
				renderDesc.colorAttachments.push_back({ color.view, color.loadOp, color.storeOp, color.clearColor });
			}

			if (hasDepth) {
				VkImageLayout depthLayout = (pass.depthAttachment->storeOp == VK_ATTACHMENT_STORE_OP_NONE) ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
				renderDesc.depthAttachment = DepthAttachmentDesc{ pass.depthAttachment->view, pass.depthAttachment->loadOp, pass.depthAttachment->storeOp, pass.depthAttachment->clearDepth, depthLayout };
			}

			ctx.begin_rendering(renderDesc);
			ctx.set_viewport_scissor(renderDesc.renderExtent);

			if (pass.executeCallback) {
				pass.executeCallback(ctx);
			}

			ctx.end_rendering();
		}
		else {
			if (pass.executeCallback) {
				pass.executeCallback(ctx);
			}
		}
	}
}

void RenderGraph::transition_texture(VkCommandBuffer cmd, const std::string & name, VkImageLayout newLayout)
{
	RGTextureState* tex = get_texture(name);
	if (tex && tex->currentLayout != newLayout) {
		vkutil::image_mem_barrier2(cmd, tex->image, tex->format, tex->currentLayout, newLayout, tex->layerCount, tex->mipLevels, tex->baseMiplevel);
		tex->currentLayout = newLayout;
	}
}

void RenderGraph::transition_buffer(VkCommandBuffer cmd, const std::string & name, VkPipelineStageFlags2 newStage)
{
	RGBufferState* buf = get_buffer(name);
	if (buf && buf->currentStage != newStage) {
		vkutil::buffer_mem_barrier2(cmd, buf->buffer, buf->currentStage, newStage);
		buf->currentStage = newStage;
	}
}

RGTextureState* RenderGraph::get_texture(const std::string & name)
{
	auto it = _textures.find(name);
	return it != _textures.end() ? &it->second : nullptr;
}

RGBufferState* RenderGraph::get_buffer(const std::string& name)
{
	auto it = _buffers.find(name);
	return it != _buffers.end() ? &it->second : nullptr;
}
