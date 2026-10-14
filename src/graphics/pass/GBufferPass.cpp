#include "GBufferPass.h"

#include <graphics/reflection/DescriptorBinder.h>
#include <graphics/Renderer.h>
#include <resources/GlobalGeometryPool.h>
#include <resources/DefaultResources.h>

void GBufferPass::init(VulkanCore* vulkanCore, ResourceManager* resourceManager)
{
	_vulkanCore = vulkanCore;
	_resourceManager = resourceManager;
	_depthFormat = VK_FORMAT_D32_SFLOAT;
}

void GBufferPass::cleanup()
{
	_vulkanCore = nullptr;
}

void GBufferPass::add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view)
{
	auto& targets = blackboard.get<SceneRenderContext>();
	VkExtent3D extent = { view.drawExtent.width, view.drawExtent.height, 1 };

	RGTextureDesc hdrGbufferDesc{
		.extent = extent,
		.format = VK_FORMAT_R16G16B16A16_SFLOAT,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
	};
	RGTextureDesc albedoDesc = hdrGbufferDesc;
	albedoDesc.format = VK_FORMAT_R8G8B8A8_SRGB;

	RGTextureDesc metalRoughDesc = hdrGbufferDesc;
	metalRoughDesc.format = VK_FORMAT_R8G8B8A8_UNORM;

	RGTextureDesc depthDesc{
		.extent = extent,
		.format = _depthFormat,
		.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
	};

	targets.gPosition = graph.create_texture("GBufferPosition", hdrGbufferDesc);
	targets.gNormal = graph.create_texture("GBufferNormal", hdrGbufferDesc);
	targets.gAlbedo = graph.create_texture("GBufferAlbedo", albedoDesc);
	targets.gMetalRough = graph.create_texture("GBufferMetalRough", metalRoughDesc);
	targets.gEmissive = graph.create_texture("GBufferEmissive", hdrGbufferDesc);
	//targets.sceneDepth = graph.create_texture("SceneDepth", depthDesc); // set in main renderer class

	graph.add_pass<GeometryPassParameters>("GBufferPass",
		[&](RGPassBuilder& builder, GeometryPassParameters& params) {
			params.gpuObjectBuffer = builder.read_buffer(targets.gpuObjectBuffer, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
			params.gpuInstanceBuffer = builder.read_buffer(targets.gpuInstanceBuffer, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
			params.gpuIndirectBuffer = builder.read_buffer(targets.gpuIndirectBuffer, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
			params.gpuCountBuffer = builder.read_buffer(targets.gpuCountBuffer, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
			params.materialBuffer = builder.read_buffer(targets.materialBuffer, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

			params.gPosition = builder.set_color_attachment(targets.gPosition, VK_ATTACHMENT_LOAD_OP_CLEAR, { 0.0f, 0.0f, 0.0f, 0.0f });
			params.gNormal = builder.set_color_attachment(targets.gNormal, VK_ATTACHMENT_LOAD_OP_CLEAR, { 0.0f, 0.0f, 0.0f, 0.0f });
			params.gAlbedo = builder.set_color_attachment(targets.gAlbedo, VK_ATTACHMENT_LOAD_OP_CLEAR, { 0.0f, 0.0f, 0.0f, 0.0f });
			params.gMetalRough = builder.set_color_attachment(targets.gMetalRough, VK_ATTACHMENT_LOAD_OP_CLEAR, { 0.0f, 0.0f, 0.0f, 0.0f });
			params.gEmissive = builder.set_color_attachment(targets.gEmissive, VK_ATTACHMENT_LOAD_OP_CLEAR, { 0.0f, 0.0f, 0.0f, 0.0f });
			params.depth = builder.set_depth_attachment(targets.sceneDepth, VK_ATTACHMENT_LOAD_OP_CLEAR, 0.0f);
			builder.set_extent(view.drawExtent);
		},
		[this, &view, &graph](const GeometryPassParameters& params, RenderContext& ctx) {
			const DrawContext& drawCtx = view.drawCtx;
			if (drawCtx.indirectDrawCount == 0 || drawCtx.opaqueBatches.empty()) {
				return;
			}

			ResourceHandle<MaterialTemplate>& pbrTemplate = DefaultResources::get().defaultPBRTemplate;
			if (!pbrTemplate.is_valid())
				return;

			VkDescriptorSetLayout layout0 = _resourceManager->get_pipeline_layout_cache().get_descriptor_set_layout(pbrTemplate->get_reflection().sets.at(0), false);
			VkDescriptorSet globalSet = view.frameData._frameDescriptors.allocate(_vulkanCore->get_device(), layout0);

			// update global descriptor set
			std::unordered_map<std::string, VkDescriptorBufferInfo> buffers = {
				{ "sceneData", { view.frameData.sceneDataBuffer->get_buffer(), 0, sizeof(GPUSceneData) }},
				{"objectBuffer", { graph.get_buffer(params.gpuObjectBuffer), 0, sizeof(GPUObjectData) * drawCtx.indirectDrawCount }},
				{"instanceBuffer", { graph.get_buffer(params.gpuInstanceBuffer), 0, sizeof(uint32_t) * drawCtx.indirectDrawCount }},
				{"materialBuffer", { graph.get_buffer(params.materialBuffer), 0, view.frameData._materialDataBuffer->get_size() }}
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
				.colorAttachmentFormats = { 
					VK_FORMAT_R16G16B16A16_SFLOAT, 
					VK_FORMAT_R16G16B16A16_SFLOAT, 
					VK_FORMAT_R8G8B8A8_SRGB, 
					VK_FORMAT_R8G8B8A8_UNORM, 
					VK_FORMAT_R16G16B16A16_SFLOAT 
				},
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
		});
}
