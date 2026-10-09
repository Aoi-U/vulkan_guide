#include "PointShadowPass.h"

#include <utils/vk_descriptors.h>
#include <graphics/Renderer.h>
#include <chrono>
#include <glm/gtc/matrix_transform.hpp>

void PointShadowPass::init(VulkanCore* vulkanCore, ResourceManager* resourceManager, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat)
{
	_vulkanCore = vulkanCore;

	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // binding 0: storage buffer for object data
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // binding 1: storage buffer for instance index
	_descriptorLayout = builder.build(_vulkanCore->get_device(), VK_SHADER_STAGE_VERTEX_BIT);

	auto vert = resourceManager->load<Shader>("point_shadow_vert", VK_SHADER_STAGE_VERTEX_BIT, "main", "../../shaders/point_shadow.vert.spv");
	auto frag = resourceManager->load<Shader>("point_shadow_frag", VK_SHADER_STAGE_FRAGMENT_BIT, "main", "../../shaders/point_shadow.frag.spv");

	GraphicsPipelineDesc pipeDesc{
		.shaders = {vert, frag},
		.descriptorLayouts = {_descriptorLayout},
		.pushConstants = {
			{
				.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				.offset = 0,
				.size = sizeof(PointShadowPushConstants)
			}
		},
		.depthAttachmentFormat = depthFormat,
		.depthState = {.depthTest = true, .depthWrite = true, .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL},
		.rasterState = {.cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_CLOCKWISE, .polygonMode = VK_POLYGON_MODE_FILL }
	};
	_pipeline = GraphicsPipeline(_vulkanCore->get_device(), pipeDesc);
}

void PointShadowPass::cleanup()
{
	_pipeline.cleanup();
	if (_descriptorLayout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(_vulkanCore->get_device(), _descriptorLayout, nullptr);
		_descriptorLayout = VK_NULL_HANDLE;
	}
	_vulkanCore = nullptr;
}

void PointShadowPass::render(RenderContext& ctx, FrameData& frameData, const std::unordered_map<std::string, ResourceHandle<Texture>>& renderTargets, const DrawContext& drawCtx, const GPUSceneData& sceneData, const PointShadowPassParameters& params, const RenderGraph& graph)
{
	int shadowLightCount = static_cast<int>(sceneData.lightCount.y);
	VkExtent2D extent = renderTargets.at("PointShadowMap")->get_extent_2d();

	// begin clock
	auto start = std::chrono::system_clock::now();

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
		VkDescriptorSet shadowDescriptor = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _descriptorLayout);

		// update the descriptor set 
		DescriptorWriter writer;
		writer.write_buffer(0, graph.get_buffer(params.gpuObjectBuffer), sizeof(GPUObjectData) * drawCtx.indirectDrawCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
		writer.write_buffer(1, graph.get_buffer(params.gpuInstanceBuffer), sizeof(uint32_t) * drawCtx.indirectDrawCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
		writer.update_set(_vulkanCore->get_device(), shadowDescriptor);

		// render each cubemap face
		for (uint32_t face = 0; face < 6; face++) {
			RenderDesc desc{};
			desc.renderExtent = extent;
			desc.depthAttachment = DepthAttachmentDesc{
				.view = renderTargets.at("PointShadowMap")->get_face_view(face),
				.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
				.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
				.clearDepth = 1.0f,
				.layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL
			};

			ctx.begin_rendering(desc);
			ctx.set_viewport_scissor(extent);
			ctx.bind_pipeline(_pipeline);
			ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.get_layout(), 0, shadowDescriptor);

			// set push constants
			PointShadowPushConstants push{
				.lightViewproj = shadowTransforms[face],
				.lightPos = glm::vec4(lightPos, farPlane)
			};
			ctx.set_push_constants(_pipeline.get_layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, push);

			VkBuffer lastIndexBuffer = VK_NULL_HANDLE;
			for (const IndirectBatch& batch : drawCtx.opaqueBatches) {
				// bind index buffer if needed
				if (batch.indexBuffer != lastIndexBuffer) {
					lastIndexBuffer = batch.indexBuffer;
					ctx.bind_index_buffer(batch.indexBuffer);
				}

				// draw batch
				VkDeviceSize indirectOffset = batch.first * sizeof(VkDrawIndexedIndirectCommand);
				VkDeviceSize countOffset = batch.batchIndex * sizeof(uint32_t);
				ctx.draw_indexed_indirect_count(graph.get_buffer(params.gpuIndirectBuffer), indirectOffset, graph.get_buffer(params.gpuCountBuffer), countOffset, batch.count, sizeof(VkDrawIndexedIndirectCommand));
			}
			ctx.end_rendering();
		}
	}

	auto end = std::chrono::system_clock::now();
	_stats.clear();
	_stats.name = "Point Shadow Pass";
	_stats.passTimeMs = std::chrono::duration<float, std::milli>(end - start).count();
}
