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
	if (ctx.indirectDrawCount == 0 || ctx.gpuIndirectBuffer.buffer == VK_NULL_HANDLE) {
		return;
	}

	// begin clock
	auto start = std::chrono::system_clock::now();

	// allocate a new uniform buffer for scene data
	AllocatedBuffer gpuSceneDataBuffer = _vulkanCore->create_buffer(sizeof(GPUSceneData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	// add to deletion queue of this frame so it gets deleted once its been used
	frameData._deletionQueue.push_function([vulkanCore = _vulkanCore, gpuSceneDataBuffer]() {
		vulkanCore->destroy_buffer(gpuSceneDataBuffer);
		});

	// write scene data to the buffer
	GPUSceneData* sceneUniformData = (GPUSceneData*)gpuSceneDataBuffer.info.pMappedData;
	*sceneUniformData = sceneData;

	// create a descriptor set that binds the scene data buffer and update it
	VkDescriptorSet globalDescriptor = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _sceneDataDescriptorLayout);

	DescriptorWriter writer;
	writer.write_buffer(0, gpuSceneDataBuffer.buffer, sizeof(GPUSceneData), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	writer.write_buffer(1, ctx.gpuObjectBuffer.buffer, sizeof(GPUObjectData) * ctx.indirectDrawCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.write_buffer(2, ctx.gpuInstanceBuffer.buffer, sizeof(uint32_t) * ctx.indirectDrawCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.update_set(_vulkanCore->get_device(), globalDescriptor);

	// set dynamic viewport and scissor
	VkViewport viewport{0.f, 0.f, static_cast<float>(drawExtent.width), static_cast<float>(drawExtent.height), 0.f, 1.f};
	vkCmdSetViewport(cmd, 0, 1, &viewport);

	VkRect2D scissor{{0, 0}, drawExtent};
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	MaterialPipeline* lastPipeline = nullptr;
	MaterialInstance* lastMaterial = nullptr;
	VkBuffer lastIndexBuffer = VK_NULL_HANDLE;

	for (uint32_t i = 0; i < ctx.indirectDrawCount; i++) {
		const RenderObject& obj = ctx.opaqueObjects[i];

		if (obj.material != lastMaterial) {
			lastMaterial = obj.material;

			// rebind pipeline and descriptors if the material changed
			if (obj.material->pipeline != lastPipeline) {
				lastPipeline = obj.material->pipeline;

				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, obj.material->pipeline->pipeline);
				// bind global scene descriptor set
				vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, obj.material->pipeline->layout, 0, 1, &globalDescriptor, 0, nullptr);
			}
			// bind material descriptor set
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lastPipeline->layout, 1, 1, &obj.material->materialSet, 0, nullptr);
		}
		
		// rebind index buffer if needed
		if (obj.indexBuffer != lastIndexBuffer) {
			lastIndexBuffer = obj.indexBuffer;
			vkCmdBindIndexBuffer(cmd, obj.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
		}

		VkDeviceSize indirectOffset = i * sizeof(DrawIndexedIndirectCommand);
		vkCmdDrawIndexedIndirect(cmd, ctx.gpuIndirectBuffer.buffer, indirectOffset, 1, sizeof(DrawIndexedIndirectCommand));
	}


	auto end = std::chrono::system_clock::now();
	_stats.passTime = std::chrono::duration<float, std::milli>(end - start).count();
}
