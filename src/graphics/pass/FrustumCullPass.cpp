#include "FrustumCullPass.h"
#include <utils/vk_descriptors.h>
#include <utils/vk_initializers.h>
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

	DescriptorLayoutBuilder cullLayoutBuilder;
	cullLayoutBuilder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	cullLayoutBuilder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	cullLayoutBuilder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	cullLayoutBuilder.add_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); 
	_cullDescriptorLayout = cullLayoutBuilder.build(_vulkanCore->get_device(), VK_SHADER_STAGE_COMPUTE_BIT);

	VkPushConstantRange cullPushRange{
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			.offset = 0,
			.size = sizeof(CullPushConstants)
	};

	VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipeline_layout_create_info();
	layoutInfo.setLayoutCount = 1;
	layoutInfo.pSetLayouts = &_cullDescriptorLayout;
	layoutInfo.pushConstantRangeCount = 1;
	layoutInfo.pPushConstantRanges = &cullPushRange;
	VK_CHECK(vkCreatePipelineLayout(_vulkanCore->get_device(), &layoutInfo, nullptr, &_cullPipelineLayout));

	VkComputePipelineCreateInfo computeInfo{
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = vkinit::pipeline_shader_stage_create_info(VK_SHADER_STAGE_COMPUTE_BIT, cullShader),
			.layout = _cullPipelineLayout
	};

	VK_CHECK(vkCreateComputePipelines(_vulkanCore->get_device(), VK_NULL_HANDLE, 1, &computeInfo, nullptr, &_cullPipeline));
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

	auto* statsRead = static_cast<CullStatsData*>(frameData._cullStatsBuffer.info.pMappedData);
	_stats.drawcallCount = statsRead->visibleDraws;
	_stats.triangleCount = statsRead->visibleTriangles;

	// Clear stats buffer for the new frame
	statsRead->visibleDraws = 0;
	statsRead->visibleTriangles = 0;

	auto start = std::chrono::system_clock::now();
	const uint32_t count = static_cast<uint32_t>(ctx.opaqueObjects.size());
	ctx.indirectDrawCount = count;

	// allocate GPU buffers
	ctx.gpuObjectBuffer = _vulkanCore->create_buffer(sizeof(GPUObjectData) * count,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	ctx.gpuInstanceBuffer = _vulkanCore->create_buffer(sizeof(uint32_t) * count,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	ctx.gpuIndirectBuffer = _vulkanCore->create_buffer(sizeof(DrawIndexedIndirectCommand) * count,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);


	frameData._deletionQueue.push_function([v = _vulkanCore, obj = ctx.gpuObjectBuffer, inst = ctx.gpuInstanceBuffer, ind = ctx.gpuIndirectBuffer]() {
		v->destroy_buffer(obj);
		v->destroy_buffer(inst);
		v->destroy_buffer(ind);
		});

	auto* mappedObjects = static_cast<GPUObjectData*>(ctx.gpuObjectBuffer.info.pMappedData);
	auto* mappedCommands = static_cast<DrawIndexedIndirectCommand*>(ctx.gpuIndirectBuffer.info.pMappedData);

	for (uint32_t i = 0; i < count; ++i) {
		const auto& obj = ctx.opaqueObjects[i];
		mappedObjects[i].worldMatrix = obj.transform;
		mappedObjects[i].sphereBounds = glm::vec4(obj.bounds.origin, obj.bounds.sphereRadius);
		mappedObjects[i].vertexBufferAddress = obj.vertexBufferAddress;
		mappedObjects[i].padding[0] = 0;
		mappedObjects[i].padding[1] = 0;

		mappedCommands[i].indexCount = obj.indexCount;
		mappedCommands[i].instanceCount = 0;
		mappedCommands[i].firstIndex = obj.firstIndex;
		mappedCommands[i].vertexOffset = 0;
		mappedCommands[i].firstInstance = i;
	}

	VkDescriptorSet cullSet = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _cullDescriptorLayout);
	DescriptorWriter writer;
	writer.write_buffer(0, ctx.gpuObjectBuffer.buffer, sizeof(GPUObjectData) * count, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.write_buffer(1, ctx.gpuInstanceBuffer.buffer, sizeof(uint32_t) * count, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.write_buffer(2, ctx.gpuIndirectBuffer.buffer, sizeof(DrawIndexedIndirectCommand) * count, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.write_buffer(3, frameData._cullStatsBuffer.buffer, sizeof(CullStatsData), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.update_set(_vulkanCore->get_device(), cullSet);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _cullPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _cullPipelineLayout, 0, 1, &cullSet, 0, nullptr);

	CullPushConstants cullPush{};
	get_frustum_planes(sceneData.viewproj, cullPush.frustumPlanes);
	cullPush.drawCount = count;
	cullPush.cullMode = 1;
	vkCmdPushConstants(cmd, _cullPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CullPushConstants), &cullPush);

	vkCmdDispatch(cmd, (count + 255) / 256, 1, 1);

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
	_stats.passTime = std::chrono::duration<float, std::milli>(end - start).count();
}