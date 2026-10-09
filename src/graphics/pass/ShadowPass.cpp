#include "ShadowPass.h"
#include <utils/vk_descriptors.h>
#include <graphics/Renderer.h>
#include <chrono>

void ShadowPass::init(VulkanCore* vulkanCore, ResourceManager* resourceManager, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat)
{
	_vulkanCore = vulkanCore;
	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // binding 0: storage buffer for object data
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // binding 1: storage buffer for instance index
	_descriptorLayout = builder.build(_vulkanCore->get_device(), VK_SHADER_STAGE_VERTEX_BIT);

	auto vertShader = resourceManager->load<Shader>("shadow_vert", VK_SHADER_STAGE_VERTEX_BIT, "main", "../../shaders/shadow.vert.spv");

	GraphicsPipelineDesc pipeDesc{
		.shaders = { vertShader },
		.descriptorLayouts = { _descriptorLayout },
		.pushConstants = { {.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = sizeof(glm::mat4) } },
		.depthAttachmentFormat = depthFormat,
		.depthState = {.depthTest = true, .depthWrite = true, .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL },
		.rasterState = {.cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_CLOCKWISE }
	};

	_pipeline = GraphicsPipeline(_vulkanCore->get_device(), pipeDesc);
}

void ShadowPass::cleanup()
{
	_pipeline.cleanup();
	if (_descriptorLayout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(_vulkanCore->get_device(), _descriptorLayout, nullptr);
		_descriptorLayout = VK_NULL_HANDLE;
	}
	_vulkanCore = nullptr;
}

void ShadowPass::render(RenderContext& ctx, FrameData& frameData, DrawContext& drawCtx, const GPUSceneData& sceneData, const ShadowPassParameters& params, const RenderGraph& graph)
{
	if (drawCtx.indirectDrawCount == 0 || drawCtx.opaqueBatches.empty()) {
		return;
	}

	// begin clock
	auto start = std::chrono::system_clock::now();

	// bind pipeline and push constants
	ctx.bind_pipeline(_pipeline);
	ctx.set_push_constants(_pipeline.get_layout(), VK_SHADER_STAGE_VERTEX_BIT, sceneData.sunlightViewproj);

	VkDescriptorSet shadowDescriptor = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _descriptorLayout);
	DescriptorWriter writer;
	writer.write_buffer(0, graph.get_buffer(params.gpuObjectBuffer), sizeof(GPUObjectData) * drawCtx.indirectDrawCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.write_buffer(1, graph.get_buffer(params.gpuInstanceBuffer), sizeof(uint32_t) * drawCtx.indirectDrawCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.update_set(_vulkanCore->get_device(), shadowDescriptor);

	ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.get_layout(), 0, shadowDescriptor);

	VkBuffer lastIndexBuffer = VK_NULL_HANDLE;
	for (const IndirectBatch& batch : drawCtx.opaqueBatches) {
		// rebind index buffer if needed
		if (batch.indexBuffer != lastIndexBuffer) {
			lastIndexBuffer = batch.indexBuffer;
			ctx.bind_index_buffer(batch.indexBuffer);
		}

		// draw batch
		VkDeviceSize indirectOffset = batch.first * sizeof(VkDrawIndexedIndirectCommand);
		VkDeviceSize countOffset = batch.batchIndex * sizeof(uint32_t);
		ctx.draw_indexed_indirect_count(graph.get_buffer(params.gpuIndirectBuffer), indirectOffset, graph.get_buffer(params.gpuCountBuffer), countOffset, batch.count, sizeof(VkDrawIndexedIndirectCommand));
	}

	auto end = std::chrono::system_clock::now();
	_stats.clear();
	_stats.name = "Shadow Pass";
	_stats.passTimeMs = std::chrono::duration<float, std::milli>(end - start).count();
}
