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
		.rasterState = {.cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_CLOCKWISE, .depthClamp = true },
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
		.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.mipLevels = 1,
		.arrayLayers = SHADOW_CASCADE_COUNT,
		.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY
	};
	RGTextureHandle shadowMap = graph.create_texture("ShadowMap", shadowDesc);

	graph.add_pass<ShadowPassParameters>("ShadowPass",
		[&](RGPassBuilder& builder, ShadowPassParameters& params) {
			params.gpuObjectBuffer = builder.read_buffer(targets.gpuObjectBuffer, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
			params.gpuIndirectBuffer = builder.read_buffer(targets.gpuShadowIndirectBuffer, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
			params.gpuInstanceBuffer = builder.read_buffer(targets.gpuShadowInstanceBuffer, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
			params.shadowMap = builder.write_texture(shadowMap, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
		},
		[this, &view, &graph](const ShadowPassParameters& params, RenderContext& ctx) {
			VkExtent2D extent = { 2048, 2048 };
			bool shouldDraw = (view.drawCtx.indirectDrawCount > 0 && !view.drawCtx.opaqueBatches.empty());

			VkDescriptorSet shadowSet = VK_NULL_HANDLE;
			if (shouldDraw) {
				shadowSet = view.frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _pipeline.get_descriptor_set_layout(0));
				std::unordered_map<std::string, VkDescriptorBufferInfo> buffers = {
					{"objectBuffer", { graph.get_buffer(params.gpuObjectBuffer), 0, sizeof(GPUObjectData) * view.drawCtx.indirectDrawCount }},
					{"instanceBuffer", { graph.get_buffer(params.gpuInstanceBuffer), 0, sizeof(uint32_t) * view.drawCtx.indirectDrawCount }}
				};
				DescriptorBinder::update_set(_vulkanCore->get_device(), shadowSet, _pipeline.get_reflection().sets.at(0), buffers, {});
			}

			// render each cascade layer
			for (uint32_t cascade = 0; cascade < SHADOW_CASCADE_COUNT; cascade++) {
				RenderDesc desc{
					.renderExtent = extent,
					.depthAttachment = DepthAttachmentDesc{
						.view = graph.get_image_view(params.shadowMap, cascade),
						.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
						.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
						.clearDepth = 1.0f,
						.layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL
					}
				};

				ctx.begin_rendering(desc);
				ctx.set_viewport_scissor(extent);

				if (shouldDraw) {
					ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.get_layout(), 0, shadowSet);
					ctx.bind_index_buffer(_resourceManager->get_geometry_pool().get_index_buffer(), 0, VK_INDEX_TYPE_UINT32); // bind global index buffer
					ctx.set_cull_mode(VK_CULL_MODE_NONE);

					// bind pipeline and push constants
					ctx.bind_pipeline(_pipeline);

					PushConstantDispatcher pc(_pipeline.get_reflection());
					pc.set("sunlightViewproj", view.sceneData.cascadeViewproj[cascade]);
					pc.flush(ctx.get_cmd(), _pipeline.get_layout());

					VkBuffer indirectBuf = graph.get_buffer(params.gpuIndirectBuffer);
					for (const IndirectBatch& batch : view.drawCtx.opaqueBatches) {
						// draw batch
						VkDeviceSize indirectOffset = batch.first * sizeof(VkDrawIndexedIndirectCommand);
						ctx.draw_indexed_indirect(indirectBuf, indirectOffset, batch.count, sizeof(VkDrawIndexedIndirectCommand));
					}
				}
				ctx.end_rendering();
			}
		});
	targets.dirShadowMap = shadowMap;
}

