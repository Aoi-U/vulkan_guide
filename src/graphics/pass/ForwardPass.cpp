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

void ForwardPass::render(VkCommandBuffer cmd, FrameData & frameData, const DrawContext & ctx, const GPUSceneData & sceneData, VkExtent2D drawExtent)
{	
	// reset counters
	_stats.drawcallCount = 0;
	_stats.triangleCount = 0;
	// begin clock
	auto start = std::chrono::system_clock::now();

	// frustum cull opaque objects
	std::vector<RenderObject> visibleOpaque;
	visibleOpaque.reserve(ctx.opaqueObjects.size());
	for (const auto& obj : ctx.opaqueObjects) {
		if (is_visible(obj, sceneData.viewproj)) {
			visibleOpaque.push_back(obj);
		}
	}

	// sort the opaque surfaces by material and mesh 
	std::sort(visibleOpaque.begin(), visibleOpaque.end(), [](const RenderObject& a, const RenderObject& b) {
		if (a.material->pipeline != b.material->pipeline) {
			return a.material->pipeline < b.material->pipeline;
		}

		if (a.material != b.material) {
			return a.material < b.material;
		}

		return a.indexBuffer < b.indexBuffer;
		});

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
	writer.update_set(_vulkanCore->get_device(), globalDescriptor);

	// set dynamic viewport and scissor
	VkViewport viewport{0.f, 0.f, static_cast<float>(drawExtent.width), static_cast<float>(drawExtent.height), 0.f, 1.f};
	vkCmdSetViewport(cmd, 0, 1, &viewport);

	VkRect2D scissor{{0, 0}, drawExtent};
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	MaterialPipeline* lastPipeline = nullptr;
	MaterialInstance* lastMaterial = nullptr;
	VkBuffer lastIndexBuffer = VK_NULL_HANDLE;

	auto draw = [&](const RenderObject& obj) {
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
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, obj.material->pipeline->layout, 1, 1, &obj.material->materialSet, 0, nullptr);
		}

		// rebind index buffer if needed
		if (obj.indexBuffer != lastIndexBuffer) {
			lastIndexBuffer = obj.indexBuffer;
			vkCmdBindIndexBuffer(cmd, obj.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
		}

		// set push constants for the draw call
		GPUDrawPushConstants pushConstants;
		pushConstants.vertexBufferAddress = obj.vertexBufferAddress;
		pushConstants.worldMatrix = obj.transform;
		vkCmdPushConstants(cmd, obj.material->pipeline->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GPUDrawPushConstants), &pushConstants);

		vkCmdDrawIndexed(cmd, obj.indexCount, 1, obj.firstIndex, 0, 0);

		// count drawcalls
		_stats.drawcallCount++;
		_stats.triangleCount += obj.indexCount / 3;
		};

	for (auto& r : visibleOpaque) {
		draw(r);
	}

	for (auto& r : ctx.transparentObjects) {
		if (is_visible(r, sceneData.viewproj)) {
			draw(r);
		}
	}

	auto end = std::chrono::system_clock::now();
	_stats.passTime = std::chrono::duration<float, std::milli>(end - start).count();
}

bool ForwardPass::is_visible(const RenderObject & obj, const glm::mat4 & viewProj)
{
	std::array<glm::vec3, 8> corners{
		glm::vec3 { 1, 1, 1 },
		glm::vec3 { 1, 1, -1 },
		glm::vec3 { 1, -1, 1 },
		glm::vec3 { 1, -1, -1 },
		glm::vec3 { -1, 1, 1 },
		glm::vec3 { -1, 1, -1 },
		glm::vec3 { -1, -1, 1 },
		glm::vec3 { -1, -1, -1 },
	};

	glm::mat4 mvp = viewProj * obj.transform;

	glm::vec3 min = { 1.5, 1.5, 1.5 };
	glm::vec3 max = { -1.5, -1.5, -1.5 };

	for (int c = 0; c < 8; c++) {
		// project each corner into clip space
		glm::vec4 v = mvp * glm::vec4(obj.bounds.origin + (corners[c] * obj.bounds.extents), 1.f);

		// perspective correction
		v.x = v.x / v.w;
		v.y = v.y / v.w;
		v.z = v.z / v.w;

		min = glm::min(glm::vec3{ v.x, v.y, v.z }, min);
		max = glm::max(glm::vec3{ v.x, v.y, v.z }, max);
	}

	// check if the clip space box is within the view
	if (min.z > 1.f || max.z < 0.f || min.x > 1.f || max.x < -1.f || min.y > 1.f || max.y < -1.f) {
		return false;
	}

	return true;
}
