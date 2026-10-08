#include "GBufferPass.h"

#include <utils/vk_descriptors.h>
#include <graphics/Renderer.h>
#include <chrono>

void GBufferPass::init(VulkanCore* vulkanCore, ResourceManager* resourceManager, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat)
{
	_vulkanCore = vulkanCore;

	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); // binding 0: uniform scene data
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // binding 1: mesh data
	builder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // binding 2: instance buffer
	_sceneDataDescriptorLayout = builder.build(_vulkanCore->get_device(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

	auto vert = resourceManager->load<Shader>("gbuffer_vert", VK_SHADER_STAGE_VERTEX_BIT, "main", "../../shaders/gbuffer.vert.spv");
	auto frag = resourceManager->load<Shader>("gbuffer_frag", VK_SHADER_STAGE_FRAGMENT_BIT, "main", "../../shaders/gbuffer.frag.spv");

	GraphicsPipelineDesc desc{
		.shaders = {vert, frag},
		.descriptorLayouts = {_sceneDataDescriptorLayout, materialLayout},
		.colorAttachmentFormats = {
			VK_FORMAT_R16G16B16A16_SFLOAT, // position
			VK_FORMAT_R16G16B16A16_SFLOAT, // normal
			VK_FORMAT_R8G8B8A8_UNORM, // albedo
			VK_FORMAT_R8G8B8A8_UNORM, // metal rough
			VK_FORMAT_R16G16B16A16_SFLOAT // emissive
			},
		.depthAttachmentFormat = depthFormat,
		.depthState = { .depthTest = true, .depthWrite = true, .compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL },
		.rasterState = { .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_CLOCKWISE, .polygonMode = VK_POLYGON_MODE_FILL },
		.dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE}
	};
	_pipeline = GraphicsPipeline(_vulkanCore->get_device(), desc);
}

void GBufferPass::cleanup()
{
	_pipeline.cleanup();
	if (_sceneDataDescriptorLayout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(_vulkanCore->get_device(), _sceneDataDescriptorLayout, nullptr);
		_sceneDataDescriptorLayout = VK_NULL_HANDLE;
	}
	_vulkanCore = nullptr;
}

void GBufferPass::render(RenderContext & ctx, FrameData & frameData, const std::unordered_map<std::string, ResourceHandle<Texture>> renderTargets, DrawContext & drawCtx, const GPUSceneData & sceneData)
{
	if (drawCtx.indirectDrawCount == 0 || frameData._gpuIndirectBuffer->get_buffer() == VK_NULL_HANDLE || drawCtx.opaqueBatches.empty()) {
		return;
	}

	// begin clock
	auto start = std::chrono::system_clock::now();

	VkDescriptorSet globalDescriptor = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _sceneDataDescriptorLayout);
	DescriptorWriter writer;
	writer.write_buffer(0, frameData.sceneDataBuffer->get_buffer(), sizeof(GPUSceneData), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	writer.write_buffer(1, frameData._gpuObjectBuffer->get_buffer(), sizeof(GPUObjectData) * drawCtx.indirectDrawCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.write_buffer(2, frameData._gpuInstanceBuffer->get_buffer(), sizeof(uint32_t) * drawCtx.indirectDrawCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.update_set(_vulkanCore->get_device(), globalDescriptor);

	ctx.bind_pipeline(_pipeline);
	ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.get_layout(), 0, globalDescriptor);

	VkDescriptorSet lastMaterialSet = VK_NULL_HANDLE;
	VkBuffer lastIndexBuffer = VK_NULL_HANDLE;

	for (const IndirectBatch& batch : drawCtx.opaqueBatches) {
		if (!batch.materialSet) continue;

		ctx.set_cull_mode(batch.isDoubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);

		// bind material descriptor set if needed
		if (batch.materialSet != lastMaterialSet) {
			lastMaterialSet = batch.materialSet;
			ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.get_layout(), 1, lastMaterialSet);
		}

		// bind index buffer if needed
		if (batch.indexBuffer != lastIndexBuffer) {
			lastIndexBuffer = batch.indexBuffer;
			ctx.bind_index_buffer(batch.indexBuffer);
		}

		// draw batch
		VkDeviceSize indirectOffset = batch.first * sizeof(VkDrawIndexedIndirectCommand);
		VkDeviceSize countOffset = batch.batchIndex * sizeof(uint32_t);
		ctx.draw_indexed_indirect_count(frameData._gpuIndirectBuffer->get_buffer(), indirectOffset, frameData._gpuCountBuffer->get_buffer(), countOffset, batch.count, sizeof(VkDrawIndexedIndirectCommand));
	}

	auto end = std::chrono::system_clock::now();
	_stats.clear();
	_stats.name = "Geometry Pass";
	_stats.passTimeMs = std::chrono::duration<float, std::milli>(end - start).count();
}
