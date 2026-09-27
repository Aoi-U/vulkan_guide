#pragma once

#include "ForwardPass.h"

#include <utils/vk_descriptors.h>
#include <graphics/Renderer.h>
#include <algorithm>
#include <chrono>

void ForwardPass::init(VulkanCore* vulkanCore, VkFormat colorFormat, VkFormat depthFormat)
{
	_vulkanCore = vulkanCore;
	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	builder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	_sceneDataDescriptorLayout = builder.build(_vulkanCore->get_device(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

	_metalRoughMaterial.build_pipelines(_vulkanCore->get_device(), _sceneDataDescriptorLayout, colorFormat, depthFormat);

	_stats.name = "Forward Pass";
}

void ForwardPass::cleanup()
{
	_metalRoughMaterial.clear_resources(_vulkanCore->get_device());
	
	vkDestroyDescriptorSetLayout(_vulkanCore->get_device(), _sceneDataDescriptorLayout, nullptr);

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

	MaterialPipeline* lastPipeline = nullptr;
	MaterialInstance* lastMaterial = nullptr;
	VkBuffer lastIndexBuffer = VK_NULL_HANDLE;

	uint32_t recordedBatchCommands = 0;

	for (const IndirectBatch& batch : ctx.opaqueBatches) {
		if (!batch.material || !batch.material->pipeline) {
			continue;
		}

		// rebind pipeline if needed
		if (batch.material->pipeline != lastPipeline) {
			lastPipeline = batch.material->pipeline;
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lastPipeline->pipeline);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lastPipeline->layout, 0, 1, &globalDescriptor, 0, nullptr);
		}

		// rebind material descriptor set if needed
		if (batch.material != lastMaterial) {
			lastMaterial = batch.material;
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lastPipeline->layout, 1, 1, &lastMaterial->materialSet, 0, nullptr);
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
