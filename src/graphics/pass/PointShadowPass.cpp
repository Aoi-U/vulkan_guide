#include "PointShadowPass.h"

#include <graphics/reflection/DescriptorBinder.h>
#include <graphics/reflection/PushConstantDispatcher.h>
#include <graphics/Renderer.h>
#include <resources/GlobalGeometryPool.h>
#include <chrono>
#include <glm/gtc/matrix_transform.hpp>

void PointShadowPass::init(VulkanCore* vulkanCore, ResourceManager* resourceManager, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat)
{
	_vulkanCore = vulkanCore;
	_resourceManager = resourceManager;

	auto vert = resourceManager->load<Shader>("point_shadow.vert", VK_SHADER_STAGE_VERTEX_BIT, "main");
	auto frag = resourceManager->load<Shader>("point_shadow.frag", VK_SHADER_STAGE_FRAGMENT_BIT, "main");

	GraphicsPipelineDesc pipeDesc{
		.shaders = {vert, frag},
		.depthAttachmentFormat = depthFormat,
		.depthState = {.depthTest = true, .depthWrite = true, .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL},
		.rasterState = {.cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_CLOCKWISE, .polygonMode = VK_POLYGON_MODE_FILL }
	};
	_pipeline = GraphicsPipeline(_vulkanCore->get_device(), resourceManager, pipeDesc);
}

void PointShadowPass::cleanup()
{
	_pipeline.cleanup();
	_vulkanCore = nullptr;
}

void PointShadowPass::render(RenderContext& ctx, FrameData& frameData, const DrawContext& drawCtx, const GPUSceneData& sceneData, const PointShadowPassParameters& params, const RenderGraph& graph)
{
	int shadowLightCount = static_cast<int>(sceneData.lightCount.y);
	VkExtent3D extent3d = graph.get_extent(params.pointShadowMap);
	VkExtent2D extent = { extent3d.width, extent3d.height };

	// setup 6 face transforms if valid shadow casting light is present
	std::array<glm::mat4, 6> shadowTransforms{};
	glm::vec3 lightPos(0.0f);
	float farPlane = 1.0f;

	if (shadowLightCount >= 0 && shadowLightCount < static_cast<int>(sceneData.lightCount.x)) {
		const GPUPointLight& light = sceneData.pointLights[shadowLightCount];
		lightPos = glm::vec3(light.position);
		farPlane = light.position.w;

		glm::mat4 shadowProj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, farPlane);

		shadowTransforms[0] = shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
		shadowTransforms[1] = shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
		shadowTransforms[2] = shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
		shadowTransforms[3] = shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
		shadowTransforms[4] = shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, -1.0f, 0.0f));
		shadowTransforms[5] = shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f));
	}

	if (drawCtx.indirectDrawCount > 0 && shadowLightCount >= 0 && !drawCtx.opaqueBatches.empty()) {
		VkDescriptorSet shadowSet = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _pipeline.get_descriptor_set_layout(0));

		std::unordered_map<std::string, VkDescriptorBufferInfo> buffers = {
			{"objectBuffer", { graph.get_buffer(params.gpuObjectBuffer), 0, sizeof(GPUObjectData) * drawCtx.indirectDrawCount }},
			{"instanceBuffer", { graph.get_buffer(params.gpuInstanceBuffer), 0, sizeof(uint32_t) * drawCtx.indirectDrawCount }}
		};
		DescriptorBinder::update_set(_vulkanCore->get_device(), shadowSet, _pipeline.get_reflection().sets.at(0), buffers, {});

		// render each cubemap face
		for (uint32_t face = 0; face < 6; face++) {
			RenderDesc desc{};
			desc.renderExtent = extent;
			desc.depthAttachment = DepthAttachmentDesc{
				.view = graph.get_image_view(params.pointShadowMap, face),
				.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
				.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
				.clearDepth = 1.0f,
				.layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL
			};

			ctx.begin_rendering(desc);
			ctx.set_viewport_scissor(extent);
			ctx.bind_pipeline(_pipeline);
			ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.get_layout(), 0, shadowSet);

			ctx.set_cull_mode(VK_CULL_MODE_NONE);

			PushConstantDispatcher pc(_pipeline.get_reflection());
			pc.set("lightViewproj", shadowTransforms[face]);
			pc.set("lightPos", glm::vec4(lightPos, farPlane));
			pc.flush(ctx.get_cmd(), _pipeline.get_layout());

			if (!drawCtx.opaqueBatches.empty()) {
				ctx.bind_index_buffer(_resourceManager->get_geometry_pool().get_index_buffer(), 0, VK_INDEX_TYPE_UINT32); // bind global index buffer

				for (const IndirectBatch& batch : drawCtx.opaqueBatches) {
					// draw batch
					VkDeviceSize indirectOffset = batch.first * sizeof(VkDrawIndexedIndirectCommand);
					VkDeviceSize countOffset = batch.batchIndex * sizeof(uint32_t);
					ctx.draw_indexed_indirect_count(graph.get_buffer(params.gpuIndirectBuffer), indirectOffset, graph.get_buffer(params.gpuCountBuffer), countOffset, batch.count, sizeof(VkDrawIndexedIndirectCommand));
				}
			}
			ctx.end_rendering();
		}
	}
}
