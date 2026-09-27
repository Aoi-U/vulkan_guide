#include "FrustumCullPass.h"
#include <utils/vk_descriptors.h>
#include <utils/vk_initializers.h>
#include <graphics/utils/vk_pipelines.h>
#include <utils/vk_utils.h>
#include <graphics/Renderer.h>
#include <chrono>

void FrustumCullPass::init(VulkanCore* vulkanCore, VkFormat colorFormat, VkFormat depthFormat) {
	_vulkanCore = vulkanCore;
	build_cull_pipeline();
	_stats.name = "Compute Frustum Cull Pass";
}

void FrustumCullPass::build_cull_pipeline() {
	VkShaderModule cullShader;
	if (!vkutil::load_shader_module("../../shaders/frustumcull.comp.spv", _vulkanCore->get_device(), &cullShader)) {
		fmt::println("Error loading frustumcull.comp.spv");
		return;
	}

	// create descriptor set layout for frustum cull pass
	DescriptorLayoutBuilder cullLayoutBuilder;
	cullLayoutBuilder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // binding 0: GPUObjectData buffer
	cullLayoutBuilder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // binding 1: final instance id buffer
	cullLayoutBuilder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // binding 2: indirect draw command buffer
	cullLayoutBuilder.add_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // binding 3: cull stats buffer
	cullLayoutBuilder.add_binding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // binding 4: batch count buffer
	_cullDescriptorLayout = cullLayoutBuilder.build(_vulkanCore->get_device(), VK_SHADER_STAGE_COMPUTE_BIT);

	VkPushConstantRange cullPushRange{
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			.offset = 0,
			.size = sizeof(CullPushConstants)
	};

	// create pipeline layout 
	VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipeline_layout_create_info();
	layoutInfo.setLayoutCount = 1;
	layoutInfo.pSetLayouts = &_cullDescriptorLayout;
	layoutInfo.pushConstantRangeCount = 1;
	layoutInfo.pPushConstantRanges = &cullPushRange;
	VK_CHECK(vkCreatePipelineLayout(_vulkanCore->get_device(), &layoutInfo, nullptr, &_cullPipelineLayout));

	PipelineBuilder pipelineBuilder;
	pipelineBuilder._pipelineLayout = _cullPipelineLayout;
	pipelineBuilder.set_compute_shaders(cullShader);
	_cullPipeline = pipelineBuilder.build_compute_pipeline(_vulkanCore->get_device());

	// cleanup shader module
	vkDestroyShaderModule(_vulkanCore->get_device(), cullShader, nullptr);
}

void FrustumCullPass::cleanup() {
	VkDevice dev = _vulkanCore->get_device();
	vkDestroyPipeline(dev, _cullPipeline, nullptr);
	vkDestroyPipelineLayout(dev, _cullPipelineLayout, nullptr);
	vkDestroyDescriptorSetLayout(dev, _cullDescriptorLayout, nullptr);
	_vulkanCore = nullptr;
}

void FrustumCullPass::render(VkCommandBuffer cmd, FrameData& frameData, DrawContext& ctx, const GPUSceneData& sceneData, VkExtent2D drawExtent) {
	if (ctx.opaqueObjects.empty()) return;

	// statistics
	{
		// read cull stats from previous frame
		auto* statsRead = static_cast<CullStatsData*>(frameData._cullStatsBuffer.info.pMappedData);
		uint32_t visibleDraws = statsRead->visibleDraws;
		uint32_t visibleTriangles = statsRead->visibleTriangles;

		// reset stats buffer for next frame
		statsRead->visibleDraws = 0;
		statsRead->visibleTriangles = 0;

		// calculate culled meshes
		const uint32_t totalMeshes = static_cast<uint32_t>(ctx.opaqueObjects.size());
		visibleDraws = std::min(visibleDraws, totalMeshes);
		uint32_t culledMeshes = totalMeshes - visibleDraws;

		_stats.clear();
		_stats.name = "Compute Frustum Cull Pass";
		_stats.add_metric("Total Meshes", totalMeshes);
		_stats.add_metric("Visible Meshes", visibleDraws);
		_stats.add_metric("Culled Meshes", culledMeshes);
		_stats.add_metric("Visible Triangles", visibleTriangles);
	}
	auto start = std::chrono::system_clock::now();

	// number of objects to draw
	const uint32_t drawCount = static_cast<uint32_t>(ctx.opaqueObjects.size());
	const uint32_t batchCount = static_cast<uint32_t>(ctx.opaqueBatches.size());
	ctx.indirectDrawCount = drawCount;

	// allocate GPU buffers
	const size_t objectBufferSize = sizeof(GPUObjectData) * drawCount;
	const size_t instanceBufferSize = sizeof(uint32_t) * drawCount;
	const size_t indirectBufferSize = sizeof(VkDrawIndexedIndirectCommand) * drawCount;
	const size_t countBufferSize = sizeof(uint32_t) * batchCount;

	_vulkanCore->check_and_resize_buffer(frameData._gpuObjectBuffer, objectBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	_vulkanCore->check_and_resize_buffer(frameData._gpuInstanceBuffer, instanceBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	_vulkanCore->check_and_resize_buffer(frameData._gpuIndirectBuffer, indirectBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	_vulkanCore->check_and_resize_buffer(frameData._gpuCountBuffer, countBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	memset(frameData._gpuCountBuffer.info.pMappedData, 0, sizeof(uint32_t) * batchCount);

	// get mapped pointers to the buffers
	auto* mappedObjects = static_cast<GPUObjectData*>(frameData._gpuObjectBuffer.info.pMappedData);
	auto* mappedCommands = static_cast<VkDrawIndexedIndirectCommand*>(frameData._gpuIndirectBuffer.info.pMappedData);

	// fill object and indirect command buffers with data
	for (uint32_t b = 0; b < batchCount; ++b) {
		const auto& batch = ctx.opaqueBatches[b];
		const uint32_t batchEnd = batch.first + batch.count;

		for (uint32_t i = batch.first; i < batchEnd; ++i) {
			const RenderObject& obj = ctx.opaqueObjects[i];

			mappedObjects[i] = GPUObjectData{
				.worldMatrix = obj.transform,
				.sphereBounds = glm::vec4(obj.bounds.origin, obj.bounds.sphereRadius),
				.vertexBufferAddress = obj.vertexBufferAddress,
				.batchIndex = b,
				.batchOffset = batch.first
			};

			mappedCommands[i] = VkDrawIndexedIndirectCommand{
				.indexCount = obj.indexCount,
				.instanceCount = 0,
				.firstIndex = obj.firstIndex,
				.vertexOffset = 0,
				.firstInstance = i
			};
		}
	}

	VkDescriptorSet cullSet = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _cullDescriptorLayout);

	// update descriptor set with the new buffers
	DescriptorWriter writer;
	writer.write_buffer(0, frameData._gpuObjectBuffer.buffer, sizeof(GPUObjectData) * drawCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.write_buffer(1, frameData._gpuInstanceBuffer.buffer, sizeof(uint32_t) * drawCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.write_buffer(2, frameData._gpuIndirectBuffer.buffer, sizeof(VkDrawIndexedIndirectCommand) * drawCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.write_buffer(3, frameData._cullStatsBuffer.buffer, sizeof(CullStatsData), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.write_buffer(4, frameData._gpuCountBuffer.buffer, sizeof(uint32_t) * batchCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.update_set(_vulkanCore->get_device(), cullSet);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _cullPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _cullPipelineLayout, 0, 1, &cullSet, 0, nullptr);

	// set push constants 
	CullPushConstants cullPush{};
	get_frustum_planes(sceneData.viewproj, cullPush.frustumPlanes);
	cullPush.drawCount = drawCount;
	cullPush.cullMode = 1;
	vkCmdPushConstants(cmd, _cullPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CullPushConstants), &cullPush);

	vkCmdDispatch(cmd, (drawCount + 255) / 256, 1, 1);

	// memory barrier to guarantee compute writes are visible to indirect command dispatch and vertex shader reads
	VkMemoryBarrier2 barrier{
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
		.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
		.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT
	};
	VkDependencyInfo depInfo{
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.memoryBarrierCount = 1,
		.pMemoryBarriers = &barrier
	};
	vkCmdPipelineBarrier2(cmd, &depInfo);

	auto end = std::chrono::system_clock::now();
	_stats.passTimeMs = std::chrono::duration<float, std::milli>(end - start).count();
}