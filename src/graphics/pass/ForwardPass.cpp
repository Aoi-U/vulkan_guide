#pragma once

#include "ForwardPass.h"

#include <utils/vk_descriptors.h>
#include <graphics/Renderer.h>
#include <graphics/utils/vk_pipelines.h>
#include <graphics/utils/vk_utils.h>
#include <graphics/utils/vk_initializers.h>
#include <algorithm>
#include <chrono>

void ForwardPass::init(VulkanCore* vulkanCore, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat)
{
	_vulkanCore = vulkanCore;
	_materialDescriptorLayout = materialLayout;

	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	builder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	_sceneDataDescriptorLayout = builder.build(_vulkanCore->get_device(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

	build_pipelines(colorFormat, depthFormat);

	_stats.name = "Forward Pass";
}

void ForwardPass::cleanup()
{
	VkDevice device = _vulkanCore->get_device();
	vkDestroyPipeline(device, _opaquePipeline.pipeline, nullptr);
	vkDestroyPipeline(device, _transparentPipeline.pipeline, nullptr);
	vkDestroyPipelineLayout(device, _opaquePipeline.layout, nullptr);

	if (_sceneDataDescriptorLayout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(device, _sceneDataDescriptorLayout, nullptr);
		_sceneDataDescriptorLayout = VK_NULL_HANDLE;
	}
	_vulkanCore = nullptr;
}

void ForwardPass::render(VkCommandBuffer cmd, FrameData & frameData, DrawContext & ctx, const GPUSceneData & sceneData, VkExtent2D drawExtent)
{	
	if (ctx.indirectDrawCount == 0 || frameData._gpuIndirectBuffer.buffer == VK_NULL_HANDLE || ctx.opaqueBatches.empty()) {
		return;
	}

	// begin clock
	auto start = std::chrono::system_clock::now();

	// allocate a new uniform buffer for scene data
	_vulkanCore->check_and_resize_buffer(frameData.sceneDataBuffer, sizeof(GPUSceneData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	// write scene data to the buffer
	GPUSceneData* sceneUniformData = (GPUSceneData*)frameData.sceneDataBuffer.info.pMappedData;
	*sceneUniformData = sceneData;

	// create a descriptor set that binds the scene data buffer and update it
	VkDescriptorSet globalDescriptor = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _sceneDataDescriptorLayout);

	DescriptorWriter writer;
	writer.write_buffer(0, frameData.sceneDataBuffer.buffer, sizeof(GPUSceneData), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	writer.write_buffer(1, frameData._gpuObjectBuffer.buffer, sizeof(GPUObjectData) * ctx.indirectDrawCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.write_buffer(2, frameData._gpuInstanceBuffer.buffer, sizeof(uint32_t) * ctx.indirectDrawCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.update_set(_vulkanCore->get_device(), globalDescriptor);

	// set dynamic viewport and scissor
	VkViewport viewport{0.f, 0.f, static_cast<float>(drawExtent.width), static_cast<float>(drawExtent.height), 0.f, 1.f};
	vkCmdSetViewport(cmd, 0, 1, &viewport);

	VkRect2D scissor{{0, 0}, drawExtent};
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _opaquePipeline.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _opaquePipeline.layout, 0, 1, &globalDescriptor, 0, nullptr);

	MaterialInstance* lastMaterial = nullptr;
	VkBuffer lastIndexBuffer = VK_NULL_HANDLE;

	uint32_t recordedBatchCommands = 0;

	for (const IndirectBatch& batch : ctx.opaqueBatches) {
		if (!batch.material) continue;	

		// rebind material descriptor set if needed
		if (batch.material != lastMaterial) {
			lastMaterial = batch.material;
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _opaquePipeline.layout, 1, 1, &lastMaterial->materialSet, 0, nullptr);
		}

		// rebind index buffer if needed
		if (batch.indexBuffer != lastIndexBuffer) {
			lastIndexBuffer = batch.indexBuffer;
			vkCmdBindIndexBuffer(cmd, batch.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
		}

		// draw batch
		VkDeviceSize indirectOffset = batch.first * sizeof(VkDrawIndexedIndirectCommand);
		VkDeviceSize countOffset = batch.batchIndex * sizeof(uint32_t);

		vkCmdDrawIndexedIndirectCount(cmd, frameData._gpuIndirectBuffer.buffer, indirectOffset, frameData._gpuCountBuffer.buffer, countOffset, batch.count, sizeof(VkDrawIndexedIndirectCommand));
		recordedBatchCommands++;
	}

	auto end = std::chrono::system_clock::now(); 
	_stats.clear(); 
	_stats.name = "Forward Pass"; 
	_stats.passTimeMs = std::chrono::duration<float, std::milli>(end - start).count(); 
	_stats.add_metric("CPU Batches Recorded", recordedBatchCommands);
}

void ForwardPass::build_pipelines(VkFormat colorFormat, VkFormat depthFormat)
{
	VkDevice device = _vulkanCore->get_device();

	VkShaderModule meshFragShader;
	if (!vkutil::load_shader_module("../../shaders/mesh.frag.spv", device, &meshFragShader)) {
		fmt::println("Error when building the mesh fragment shader module");
	}
	VkShaderModule meshVertexShader;
	if (!vkutil::load_shader_module("../../shaders/mesh.vert.spv", device, &meshVertexShader)) {
		fmt::println("Error when building the mesh vertex shader module");
	}

	// set 0 = scene data, set 1 = material data from MaterialSystem
	VkDescriptorSetLayout layouts[] = { _sceneDataDescriptorLayout, _materialDescriptorLayout };
	VkPipelineLayoutCreateInfo meshLayoutInfo = vkinit::pipeline_layout_create_info();
	meshLayoutInfo.setLayoutCount = 2;
	meshLayoutInfo.pSetLayouts = layouts;

	VkPipelineLayout pipelineLayout;
	VK_CHECK(vkCreatePipelineLayout(device, &meshLayoutInfo, nullptr, &pipelineLayout));
	_opaquePipeline.layout = pipelineLayout;
	_transparentPipeline.layout = pipelineLayout;

	PipelineBuilder pipelineBuilder;
	pipelineBuilder.set_shaders(meshVertexShader, meshFragShader);
	pipelineBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	pipelineBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	pipelineBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	pipelineBuilder.set_multisampling_none();
	pipelineBuilder.disable_blending();
	pipelineBuilder.enable_depthtest(true, VK_COMPARE_OP_GREATER_OR_EQUAL);
	pipelineBuilder.set_color_attachment_format(colorFormat);
	pipelineBuilder.set_depth_format(depthFormat);
	pipelineBuilder._pipelineLayout = pipelineLayout;

	_opaquePipeline.pipeline = pipelineBuilder.build_pipeline(device);

	pipelineBuilder.enable_blending_additive();
	pipelineBuilder.enable_depthtest(false, VK_COMPARE_OP_GREATER_OR_EQUAL);
	_transparentPipeline.pipeline = pipelineBuilder.build_pipeline(device);

	vkDestroyShaderModule(device, meshFragShader, nullptr);
	vkDestroyShaderModule(device, meshVertexShader, nullptr);
}
