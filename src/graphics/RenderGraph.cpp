#include "RenderGraph.h"

#include <graphics/utils/vk_utils.h>
#include <core/Profiler.h>
#include <fmt/core.h>
#include <algorithm>

RGTextureHandle RGPassBuilder::read_texture(RGTextureHandle handle, VkPipelineStageFlags2 stage, VkImageLayout layout, VkAccessFlags2 access)
{
	if (!handle.is_valid()) {
		fmt::println("RenderGraph: attempting to read from invalid texture handle in pass {}", _passIndex);
		return handle;
	}
	// set access to shader read only
	auto& pass = _graph._passes[_passIndex];
	pass->textureAccesses.push_back({
		handle, 
		{
			.usage = RGResourceUsage::ShaderRead,
			.stage = stage,
			.access = access,
			.layout = layout
		}
		});
	_graph._textures[handle.index].refCount++;

	return handle;
}

RGTextureHandle RGPassBuilder::write_texture(RGTextureHandle handle, VkPipelineStageFlags2 stage, VkImageLayout layout, VkAccessFlags2 access)
{
	if (!handle.is_valid()) {
		fmt::println("RenderGraph: attemping to write to invalid texture handle in pass {}", _passIndex);
		return handle;
	}
	// set access to shader write only
	auto& pass = _graph._passes[_passIndex];
	pass->textureAccesses.push_back({
		handle, 
		{
			.usage = RGResourceUsage::StorageWrite,
			.stage = stage,
			.access = access,
			.layout = layout
		}
		});
	_graph._textures[handle.index].refCount++;

	return handle;
}

RGTextureHandle RGPassBuilder::set_color_attachment(RGTextureHandle handle, VkAttachmentLoadOp loadOp, VkClearColorValue clearColor)
{
	if (!handle.is_valid()) {
		fmt::println("RenderGraph: attempting to set color attachment with invalid texture handle in pass {}", _passIndex);
		return handle;
	}
	// set access to color attachment
	auto& pass = _graph._passes[_passIndex];
	pass->textureAccesses.push_back({
		handle, 
		{
			.usage = RGResourceUsage::ColorAttachment,
			.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			.access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | (loadOp == VK_ATTACHMENT_LOAD_OP_LOAD ? VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT : 0), // if loadOp is LOAD, need to read the existing content of the attachment
			.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.loadOp = loadOp,
			.clearColor = clearColor
		}
		});
	_graph._textures[handle.index].refCount++;

	return handle;
}

RGTextureHandle RGPassBuilder::set_depth_attachment(RGTextureHandle handle, VkAttachmentLoadOp loadOp, float clearDepth)
{
	if (!handle.is_valid()) {
		fmt::println("RenderGraph: attemping to set depth attachment with invalid texture handle in pass {}", _passIndex);
		return handle;
	}
	// set access to depth attachment
	auto& pass = _graph._passes[_passIndex];
	pass->textureAccesses.push_back({
		handle, 
		{
			.usage = RGResourceUsage::DepthAttachment,
			.stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
			.access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, // depth attachment needs to be read for depth testing
			.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			.loadOp = loadOp,
			.clearDepth = clearDepth
		}
		});
	_graph._textures[handle.index].refCount++;

	return handle;
}

RGBufferHandle RGPassBuilder::read_buffer(RGBufferHandle handle, VkPipelineStageFlags2 stage, VkAccessFlags access)
{
	if (!handle.is_valid()) {
		fmt::println("RenderGraph: attempting to read from invalid buffer handle in pass {}", _passIndex);
		return handle;
	}
	// set access to storage read 
	auto& pass = _graph._passes[_passIndex];
	pass->bufferAccesses.push_back({
		handle, 
		{
			.usage = RGResourceUsage::StorageRead,
			.stage = stage,
			.access = access
		}
		});
	_graph._buffers[handle.index].refCount++;

	return handle;
}

RGBufferHandle RGPassBuilder::write_buffer(RGBufferHandle handle, VkPipelineStageFlags2 stage, VkAccessFlags access)
{
	if (!handle.is_valid()) {
		fmt::println("RenderGraph: attempting to write to invalid buffer handle in pass {}", _passIndex);
		return handle;
	}
	// set access to storage write
	auto& pass = _graph._passes[_passIndex];
	pass->bufferAccesses.push_back({
		handle, 
		{
			.usage = RGResourceUsage::StorageWrite,
			.stage = stage,
			.access = access
		}
		});
	_graph._buffers[handle.index].refCount++;

	return handle;
}

void RGPassBuilder::set_extent(VkExtent2D extent)
{
	_graph._passes[_passIndex]->renderExtent = extent;
}

void RGPassBuilder::set_side_effect(bool hasSideEffect)
{
	_graph._passes[_passIndex]->hasSideEffect = hasSideEffect;
}

void RenderGraph::init(RenderTargetPool* pool)
{
	_renderTargetPool = pool;
}

void RenderGraph::reset()
{
	_passes.clear();
	_textures.clear();
	_buffers.clear();
}

RGTextureHandle RenderGraph::create_texture(std::string_view name, const RGTextureDesc& desc)
{
	uint32_t index = static_cast<uint32_t>(_textures.size());
	_textures.push_back(RGTextureNode{
		.name = std::string(name),
		.desc = desc,
		.isExternal = false
		});
	return RGTextureHandle{ index };
}

RGBufferHandle RenderGraph::create_buffer(std::string_view name, const RGBufferDesc& desc)
{
	uint32_t index = static_cast<uint32_t>(_buffers.size());
	_buffers.push_back(RGBufferNode{
		.name = std::string(name),
		.desc = desc,
		.isExternal = false
		});
	return RGBufferHandle{ index };
}

RGTextureHandle RenderGraph::import_texture(std::string_view name, const ResourceHandle<Texture>& tex, VkImageLayout currentLayout)
{
	uint32_t index = static_cast<uint32_t>(_textures.size());
	_textures.push_back(RGTextureNode{
		.name = std::string(name),
		.desc = {
			.extent = tex->get_extent(),
			.format = tex->get_format(),
			.mipLevels = tex->get_mip_levels(),
			.arrayLayers = tex->get_face_views().empty() ? 1u : static_cast<uint32_t>(tex->get_face_views().size()),
			.viewType = tex->get_face_views().size() == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : (tex->get_face_views().size() > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D),
			.samples = VK_SAMPLE_COUNT_1_BIT
		},
		.image = tex->get_image(),
		.imageView = tex->get_image_view(),
		.faceViews = tex->get_face_views(),
		.initialLayout = currentLayout,
		.currentLayout = currentLayout,
		.isExternal = true
		});
	return RGTextureHandle{ index };
}

RGBufferHandle RenderGraph::import_buffer(std::string_view name, VkBuffer buffer, VkDeviceSize size, VkPipelineStageFlags2 stage, VkAccessFlags2 access)
{
	uint32_t index = static_cast<uint32_t>(_buffers.size());
	_buffers.push_back(RGBufferNode{
		.name = std::string(name),
		.desc = {
			.size = size,
		},
		.buffer = buffer,
		.currentStage = stage,
		.currentAccess = access,
		.isExternal = true
		});
	return RGBufferHandle{ index };
}

void RenderGraph::set_texture_layout(RGTextureHandle handle, VkImageLayout layout)
{
	if (!handle.is_valid()) return;
	_textures[handle.index].currentLayout = layout;
}

RGTextureHandle RenderGraph::find_texture(std::string_view name) const
{
	for (uint32_t i = 0; i < _textures.size(); i++) {
		if (_textures[i].name == name)
			return RGTextureHandle{ i };
	}
	fmt::println("RenderGraph: texture {} not found", name);
	return RGTextureHandle{};
}

RGBufferHandle RenderGraph::find_buffer(std::string_view name) const
{
	for (uint32_t i = 0; i < _buffers.size(); i++) {
		if (_buffers[i].name == name)
			return RGBufferHandle{ i };
	}
	fmt::println("RenderGraph: buffer {} not found", name);
	return RGBufferHandle{};
}

void RenderGraph::compile(uint32_t currentFrame)
{
	cull_passes();
	calculate_lifetimes(); 
	allocate_target_resources(currentFrame);
}

void RenderGraph::execute(RenderContext& ctx)
{
	for (auto& pass : _passes) {
		if (pass->culled)
			continue;

		PROFILE_SCOPE(pass->name, ctx.get_cmd());

		dispatch_barriers(ctx, pass.get());

		RenderDesc renderDesc{};
		bool isRenderingPass = false;
		VkExtent2D extents{ 0, 0 };

		for (const auto& [handle, access] : pass->textureAccesses) {
			RGTextureNode& tex = _textures[handle.index];

			if (access.usage == RGResourceUsage::ColorAttachment) {
				ColorAttachmentDesc colorAttachment{
					.view = tex.imageView,
					.loadOp = access.loadOp,
					.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
					.clearColor = access.clearColor,
					.layout = access.layout
				};
				renderDesc.colorAttachments.push_back(colorAttachment);

				// update max color extent
				extents.width = std::max(extents.width, tex.desc.extent.width);
				extents.height = std::max(extents.height, tex.desc.extent.height);
				isRenderingPass = true;
			}
			else if (access.usage == RGResourceUsage::DepthAttachment) {
				DepthAttachmentDesc depthAttachment{
					.view = tex.imageView,
					.loadOp = access.loadOp,
					.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
					.clearDepth = access.clearDepth,
					.layout = access.layout,
				};
				renderDesc.depthAttachment = depthAttachment;

				// update max depth extent
				extents.width = std::max(extents.width, tex.desc.extent.width);
				extents.height = std::max(extents.height, tex.desc.extent.height);
				isRenderingPass = true;
			}
		}

		// set explicitly defined render extent
		if (pass->renderExtent.has_value()) {
			extents = pass->renderExtent.value();
		}
		renderDesc.renderExtent = extents;

		// begin dynamic rendering if on a render pass
		if (isRenderingPass) {
			ctx.begin_rendering(renderDesc);
			ctx.set_viewport_scissor(extents);
		}
		// execute the pass

		pass->execute(ctx, *this);

		// end dynamic rendering if on render pass
		if (isRenderingPass) {
			ctx.end_rendering();
		}
	}
}

VkImage RenderGraph::get_image(RGTextureHandle handle) const
{
	if (!handle.is_valid()) {
		fmt::println("RenderGraph: attempting to get image from invalid texture handle");
		return VK_NULL_HANDLE;
	}
	return _textures[handle.index].image;
}

VkImageView RenderGraph::get_image_view(RGTextureHandle handle, uint32_t faceIndex) const
{
	if (!handle.is_valid()) {
		fmt::println("RenderGraph: attempting to get image view from invalid texture handle");
		return VK_NULL_HANDLE;
	}
	
	const auto& tex = _textures[handle.index];
	if (faceIndex != UINT32_MAX && faceIndex < tex.faceViews.size()) {
		return tex.faceViews[faceIndex];
	}

	return tex.imageView;
}

uint32_t RenderGraph::get_array_layers(RGTextureHandle handle) const
{
	if (!handle.is_valid()) {
		return 1;
	}
	return _textures[handle.index].desc.arrayLayers;
}

VkFormat RenderGraph::get_format(RGTextureHandle handle) const
{
	if (!handle.is_valid()) {
		fmt::println("RenderGraph:: attemping to get image format from invalid texture handle");
		return VK_FORMAT_UNDEFINED;
	}
	return _textures[handle.index].desc.format;
}

VkExtent3D RenderGraph::get_extent(RGTextureHandle handle) const
{
	if (!handle.is_valid()) {
		fmt::println("RenderGraph: attempting to get image extent from invalid texture handle");
		return VkExtent3D{ 0, 0, 0 };
	}
	return _textures[handle.index].desc.extent;
}

bool RenderGraph::is_cube(RGTextureHandle handle) const
{
	if (!handle.is_valid()) {
		fmt::println("RenderGraph: attempting to check if texture is cube from invalid texture handle");
		return false;
	}
	return _textures[handle.index].desc.viewType == VK_IMAGE_VIEW_TYPE_CUBE;
}

VkBuffer RenderGraph::get_buffer(RGBufferHandle handle) const
{
	if (!handle.is_valid()) {
		fmt::println("RenderGraph: attempting to get buffer from invalid buffer handle");
		return VK_NULL_HANDLE;
	}
	return _buffers[handle.index].buffer;
}

void RenderGraph::transition_texture(VkCommandBuffer cmd, RGTextureHandle handle, VkImageLayout newLayout)
{
	if (!handle.is_valid()) {
		fmt::println("RenderGraph: attempting manual transition on invalid texture handle");
		return;
	}

	RGTextureNode& tex = _textures[handle.index];

	// transition and set new layout
	if (tex.currentLayout != newLayout && tex.image != VK_NULL_HANDLE) {
		vkutil::image_mem_barrier2(cmd, tex.image, tex.desc.format, tex.currentLayout, newLayout, tex.desc.arrayLayers, tex.desc.mipLevels, 0);
		tex.currentLayout = newLayout;
	}
}

void RenderGraph::cull_passes()
{
	// traverse backwards and cull chained unused passes
	for (int i = static_cast<int>(_passes.size()) - 1; i >= 0; i--) {
		auto& pass = _passes[i];
		if (pass->hasSideEffect) {
			pass->culled = false;
			continue;
		}

		bool hasUsedOutput = false;
		for (const auto& [handle, access] : pass->textureAccesses) {
			bool isWrite = (access.usage == RGResourceUsage::StorageWrite ||
				access.usage == RGResourceUsage::ColorAttachment ||
				access.usage == RGResourceUsage::DepthAttachment);
			if (isWrite && _textures[handle.index].refCount > 0) {
				hasUsedOutput = true;
				break;
			}
		}

		for (const auto& [handle, access] : pass->bufferAccesses) {
			bool isWrite = (access.usage == RGResourceUsage::StorageWrite);
			if (isWrite && _buffers[handle.index].refCount > 0) {
				hasUsedOutput = true;
				break;
			}
		}

		if (!hasUsedOutput) {
			pass->culled = true;

			// decrement ref counts of inputs since this pass is culled
			for (const auto& [handle, access] : pass->textureAccesses) {
				if (access.usage == RGResourceUsage::ShaderRead && _textures[handle.index].refCount > 0) {
					_textures[handle.index].refCount--;
				}
			}
			for (const auto& [handle, access] : pass->bufferAccesses) {
				if (access.usage == RGResourceUsage::StorageRead && _buffers[handle.index].refCount > 0) {
					_buffers[handle.index].refCount--;
				}
			}
		}
	}
}

void RenderGraph::calculate_lifetimes()
{
	for (auto& pass : _passes) {
		if (pass->culled)
			continue;

		for (const auto& [texHandle, _] : pass->textureAccesses) {
			RGTextureNode& node = _textures[texHandle.index];
			node.firstPass = std::min(node.firstPass, pass->index);
			node.lastPass = std::max(node.lastPass, pass->index);
		}

		for (const auto& [bufHandle, _] : pass->bufferAccesses) {
			RGBufferNode& node = _buffers[bufHandle.index];
			node.firstPass = std::min(node.firstPass, pass->index);
			node.lastPass = std::max(node.lastPass, pass->index);
		}
	}
}

void RenderGraph::allocate_target_resources(uint32_t currentFrame)
{
	if (!_renderTargetPool) {
		fmt::println("RenderGraph: no render target pool set, cannot allocate resources. call init()");
		return;
	}

	for (auto& tex : _textures) {
		if (tex.isExternal || tex.refCount == 0) {
			continue;
		}

		AllocationKey key{
			.extent = tex.desc.extent,
			.format = tex.desc.format,
			.usage = tex.desc.usage,
			.flags = tex.desc.flags,
			.viewType = tex.desc.viewType,
			.mipLevels = tex.desc.mipLevels,
			.arrayLayers = tex.desc.arrayLayers
		};

		PooledImage* pooledImage = _renderTargetPool->get_image(key, currentFrame);
		if (pooledImage) {
			tex.image = pooledImage->image;
			tex.imageView = pooledImage->imageView;
			tex.faceViews = pooledImage->faceViews;
			tex.currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		}
	}
}

void RenderGraph::dispatch_barriers(RenderContext& ctx, IRGPass* pass)
{
	std::vector<VkImageMemoryBarrier2> imageBarriers;
	std::vector<VkBufferMemoryBarrier2> bufferBarriers;

	// dispatch texture barriers
	for (const auto& [handle, access] : pass->textureAccesses) {
		RGTextureNode& tex = _textures[handle.index];
		if (tex.image == VK_NULL_HANDLE) {
			fmt::println("RenderGraph: texture {} has no image allocated", tex.name);
			continue;
		}

		if (tex.currentLayout != access.layout || access.usage == RGResourceUsage::StorageWrite || access.usage == RGResourceUsage::ColorAttachment) {
			VkImageAspectFlags aspectMask = (access.layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL || access.layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
			if (tex.currentLayout == access.layout) { 
				continue; 
			}

			if (access.usage != RGResourceUsage::StorageWrite && access.usage != RGResourceUsage::ColorAttachment) {
				bool isDepth = (tex.desc.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0 || 
					tex.desc.format == VK_FORMAT_D32_SFLOAT || 
					tex.desc.format == VK_FORMAT_D32_SFLOAT_S8_UINT || 
					tex.desc.format == VK_FORMAT_D24_UNORM_S8_UINT || 
					tex.desc.format == VK_FORMAT_D16_UNORM;
				aspectMask = isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
			}
			if (vkutil::has_stencil_component(tex.desc.format)) { aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT; }

			VkImageMemoryBarrier2 barrier{ 
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT, .dstStageMask = access.stage, .dstAccessMask = access.access, .oldLayout = tex.currentLayout, .newLayout = access.layout, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = tex.image, .subresourceRange = {.aspectMask = aspectMask, .baseMipLevel = 0, .levelCount = tex.desc.mipLevels, .baseArrayLayer = 0, .layerCount = tex.desc.arrayLayers } 
			};
			imageBarriers.push_back(barrier);
			tex.currentLayout = access.layout;
		}
	}

	// dispatch buffer barriers
	for (const auto& [handle, access] : pass->bufferAccesses) {
		RGBufferNode& buf = _buffers[handle.index];
		if (buf.buffer == VK_NULL_HANDLE) {
			fmt::println("RenderGraph: buffer {} has no buffer allocated", buf.name);
			continue;
		}

		if (buf.currentStage == access.stage && buf.currentAccess == access.access) {
			continue; // no barrier needed, already in correct stage and access
		}

		bool wasWritten = (buf.currentAccess & (VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT)) != 0;
		bool isWriting = (access.usage == RGResourceUsage::StorageWrite);

		if (wasWritten || isWriting) {
			VkBufferMemoryBarrier2 barrier{
				.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
				.srcStageMask = buf.currentStage == VK_PIPELINE_STAGE_2_NONE ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : buf.currentStage,
				.srcAccessMask = buf.currentAccess == VK_ACCESS_2_NONE ? VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT : buf.currentAccess,
				.dstStageMask = access.stage,
				.dstAccessMask = access.access,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.buffer = buf.buffer,
				.offset = 0,
				.size = VK_WHOLE_SIZE,
			};
			bufferBarriers.push_back(barrier);
			buf.currentStage = access.stage;
			buf.currentAccess = access.access;
		}

		// update current stage and access
		//buf.currentStage = access.stage;
		//buf.currentAccess = access.access;
	}

	if (!imageBarriers.empty() || !bufferBarriers.empty()) {
		VkDependencyInfo depInfo{ 
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, 
			.bufferMemoryBarrierCount = static_cast<uint32_t>(bufferBarriers.size()), 
			.pBufferMemoryBarriers = bufferBarriers.data(), 
			.imageMemoryBarrierCount = static_cast<uint32_t>(imageBarriers.size()), 
			.pImageMemoryBarriers = imageBarriers.data() };
		vkCmdPipelineBarrier2(ctx.get_cmd(), &depInfo);
	}
}
