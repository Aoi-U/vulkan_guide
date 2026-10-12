#include "GBufferPass.h"

#include <graphics/reflection/DescriptorBinder.h>
#include <graphics/Renderer.h>
#include <resources/GlobalGeometryPool.h>
#include <resources/DefaultResources.h>
#include <chrono>

void GBufferPass::init(VulkanCore* vulkanCore, ResourceManager* resourceManager, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat)
{
	_vulkanCore = vulkanCore;
	_resourceManager = resourceManager;
	_depthFormat = depthFormat;
}

void GBufferPass::cleanup()
{
	_vulkanCore = nullptr;
}

void GBufferPass::render(RenderContext& ctx, FrameData& frameData, DrawContext& drawCtx, const GPUSceneData& sceneData, const GeometryPassParameters& params, const RenderGraph& graph)
{
	if (drawCtx.indirectDrawCount == 0 || drawCtx.opaqueBatches.empty()) {
		return;
	}

	ResourceHandle<MaterialTemplate>& pbrTemplate = DefaultResources::get().defaultPBRTemplate;
	if (!pbrTemplate.is_valid()) 
		return;
	
	VkDescriptorSetLayout layout0 = _resourceManager->get_pipeline_layout_cache().get_descriptor_set_layout(pbrTemplate->get_reflection().sets.at(0), false);
	VkDescriptorSet globalSet = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), layout0);

	// update global descriptor set
	std::unordered_map<std::string, VkDescriptorBufferInfo> buffers = {
		{ "sceneData", { frameData.sceneDataBuffer->get_buffer(), 0, sizeof(GPUSceneData) }},
		{"objectBuffer", { graph.get_buffer(params.gpuObjectBuffer), 0, sizeof(GPUObjectData) * drawCtx.indirectDrawCount }},
		{"instanceBuffer", { graph.get_buffer(params.gpuInstanceBuffer), 0, sizeof(uint32_t) * drawCtx.indirectDrawCount }},
		{"materialBuffer", { graph.get_buffer(params.materialBuffer), 0, frameData._materialDataBuffer->get_size() }}
	};
	DescriptorBinder::update_set(_vulkanCore->get_device(), globalSet, pbrTemplate->get_reflection().sets.at(0), buffers, {});

	ctx.bind_index_buffer(_resourceManager->get_geometry_pool().get_index_buffer(), 0, VK_INDEX_TYPE_UINT32); // bind global index buffer
	
	MaterialPipelineKey pipelineKey{
		.cullMode = VK_CULL_MODE_BACK_BIT,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.blendMode = BlendMode::None,
		.depthTest = true,
		.depthWrite = true,
		.depthComareOp = VK_COMPARE_OP_GREATER_OR_EQUAL,
		.colorAttachmentFormats = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT },
		.depthAttachmentFormat = _depthFormat
	};
	VkPipeline pipeline = pbrTemplate->get_pipeline(pipelineKey);
	vkCmdBindPipeline(ctx.get_cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, pbrTemplate->get_layout(), 0, globalSet);
	ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, pbrTemplate->get_layout(), 1, drawCtx.bindlessMaterialSet);

	for (const IndirectBatch& batch : drawCtx.opaqueBatches) {
		pipelineKey.cullMode = batch.isDoubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
		
		ctx.set_cull_mode(pipelineKey.cullMode);

		// draw batch
		VkDeviceSize indirectOffset = batch.first * sizeof(VkDrawIndexedIndirectCommand); 
		VkDeviceSize countOffset = batch.batchIndex * sizeof(uint32_t); 
		ctx.draw_indexed_indirect_count(graph.get_buffer(params.gpuIndirectBuffer), indirectOffset, graph.get_buffer(params.gpuCountBuffer), countOffset, batch.count, sizeof(VkDrawIndexedIndirectCommand));
	}
}
