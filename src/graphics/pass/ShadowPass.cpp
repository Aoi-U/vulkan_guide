#include "ShadowPass.h"
#include <graphics/reflection/DescriptorBinder.h>
#include <graphics/reflection/PushConstantDispatcher.h>
#include <graphics/Renderer.h>
#include <resources/GlobalGeometryPool.h>

void ShadowPass::init(VulkanCore* vulkanCore, ResourceManager* resourceManager)
{
	_vulkanCore = vulkanCore;
	_resourceManager = resourceManager;

	auto vertShader = resourceManager->load<Shader>("shadow.vert", VK_SHADER_STAGE_VERTEX_BIT, "main");
	GraphicsPipelineDesc pipeDesc{
		.shaders = { vertShader },
		.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT,
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

void ShadowPass::add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view)
{
	auto& targets = blackboard.get<SceneRenderContext>();

	RGTextureDesc shadowDesc{
		.extent = { 2048, 2048, 1 },
		.format = VK_FORMAT_D32_SFLOAT,
		.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
	};
	RGTextureHandle shadowMap = graph.create_texture("ShadowMap", shadowDesc);

	graph.add_pass<ShadowPassParameters>("ShadowPass",
		[&](RGPassBuilder& builder, ShadowPassParameters& params) {
			params.gpuObjectBuffer = builder.read_buffer(targets.gpuObjectBuffer, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
			params.gpuIndirectBuffer = builder.read_buffer(targets.gpuIndirectBuffer, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
			params.gpuInstanceBuffer = builder.read_buffer(targets.gpuInstanceBuffer, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
			params.gpuCountBuffer = builder.read_buffer(targets.gpuCountBuffer, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
			params.shadowMap = builder.set_depth_attachment(shadowMap, VK_ATTACHMENT_LOAD_OP_CLEAR, 1.0f);
			builder.set_extent({ 2048, 2048 });
		},
		[this, &view, &graph](const ShadowPassParameters& params, RenderContext& ctx) {
			if (view.drawCtx.indirectDrawCount == 0 || view.drawCtx.opaqueBatches.empty()) {
				return;
			}

			// bind pipeline and push constants
			ctx.bind_pipeline(_pipeline);

			PushConstantDispatcher pc(_pipeline.get_reflection());
			pc.set("sunlightViewproj", view.sceneData.sunlightViewproj);
			pc.flush(ctx.get_cmd(), _pipeline.get_layout());

			VkDescriptorSet shadowSet = view.frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _pipeline.get_descriptor_set_layout(0));
			std::unordered_map<std::string, VkDescriptorBufferInfo> buffers = {
				{"objectBuffer", { graph.get_buffer(params.gpuObjectBuffer), 0, sizeof(GPUObjectData) * view.drawCtx.indirectDrawCount }},
				{"instanceBuffer", { graph.get_buffer(params.gpuInstanceBuffer), 0, sizeof(uint32_t) * view.drawCtx.indirectDrawCount }}
			};
			DescriptorBinder::update_set(_vulkanCore->get_device(), shadowSet, _pipeline.get_reflection().sets.at(0), buffers, {});
			ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.get_layout(), 0, shadowSet);

			ctx.bind_index_buffer(_resourceManager->get_geometry_pool().get_index_buffer(), 0, VK_INDEX_TYPE_UINT32); // bind global index buffer
			ctx.set_cull_mode(VK_CULL_MODE_NONE);

			for (const IndirectBatch& batch : view.drawCtx.opaqueBatches) {
				// draw batch
				VkDeviceSize indirectOffset = batch.first * sizeof(VkDrawIndexedIndirectCommand);
				VkDeviceSize countOffset = batch.batchIndex * sizeof(uint32_t);
				ctx.draw_indexed_indirect_count(graph.get_buffer(params.gpuIndirectBuffer), indirectOffset, graph.get_buffer(params.gpuCountBuffer), countOffset, batch.count, sizeof(VkDrawIndexedIndirectCommand));
			}
		});
	targets.dirShadowMap = shadowMap;
}

