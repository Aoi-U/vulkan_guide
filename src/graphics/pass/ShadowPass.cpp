#include "ShadowPass.h"
#include <graphics/reflection/DescriptorBinder.h>
#include <graphics/reflection/PushConstantDispatcher.h>
#include <graphics/Renderer.h>
#include <resources/GlobalGeometryPool.h>
#include <chrono>

void ShadowPass::init(VulkanCore* vulkanCore, ResourceManager* resourceManager, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat)
{
	_vulkanCore = vulkanCore;
	_resourceManager = resourceManager;

	auto vertShader = resourceManager->load<Shader>("shadow.vert", VK_SHADER_STAGE_VERTEX_BIT, "main");

	GraphicsPipelineDesc pipeDesc{
		.shaders = { vertShader },
		.depthAttachmentFormat = depthFormat,
		.depthState = {.depthTest = true, .depthWrite = true, .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL },
		.rasterState = {.cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_CLOCKWISE }
	};

	_pipeline = GraphicsPipeline(_vulkanCore->get_device(), _resourceManager, pipeDesc);
}

void ShadowPass::cleanup()
{
	_pipeline.cleanup();
	_vulkanCore = nullptr;
}

void ShadowPass::render(RenderContext& ctx, FrameData& frameData, DrawContext& drawCtx, const GPUSceneData& sceneData, const ShadowPassParameters& params, const RenderGraph& graph)
{
	if (drawCtx.indirectDrawCount == 0 || drawCtx.opaqueBatches.empty()) {
		return;
	}

	// bind pipeline and push constants
	ctx.bind_pipeline(_pipeline);

	PushConstantDispatcher pc(_pipeline.get_reflection());
	pc.set("sunlightViewproj", sceneData.sunlightViewproj);
	pc.flush(ctx.get_cmd(), _pipeline.get_layout());

	VkDescriptorSet shadowSet = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _pipeline.get_descriptor_set_layout(0));
	std::unordered_map<std::string, VkDescriptorBufferInfo> buffers = {
		{"objectBuffer", { graph.get_buffer(params.gpuObjectBuffer), 0, sizeof(GPUObjectData) * drawCtx.indirectDrawCount }},
		{"instanceBuffer", { graph.get_buffer(params.gpuInstanceBuffer), 0, sizeof(uint32_t) * drawCtx.indirectDrawCount }}
	};
	DescriptorBinder::update_set(_vulkanCore->get_device(), shadowSet, _pipeline.get_reflection().sets.at(0), buffers, {});
	ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.get_layout(), 0, shadowSet);

	ctx.bind_index_buffer(_resourceManager->get_geometry_pool().get_index_buffer(), 0, VK_INDEX_TYPE_UINT32); // bind global index buffer
	ctx.set_cull_mode(VK_CULL_MODE_NONE);

	for (const IndirectBatch& batch : drawCtx.opaqueBatches) {
		// draw batch
		VkDeviceSize indirectOffset = batch.first * sizeof(VkDrawIndexedIndirectCommand);
		VkDeviceSize countOffset = batch.batchIndex * sizeof(uint32_t);
		ctx.draw_indexed_indirect_count(graph.get_buffer(params.gpuIndirectBuffer), indirectOffset, graph.get_buffer(params.gpuCountBuffer), countOffset, batch.count, sizeof(VkDrawIndexedIndirectCommand));
	}
}
