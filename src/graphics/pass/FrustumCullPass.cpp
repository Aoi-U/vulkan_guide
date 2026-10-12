#include "FrustumCullPass.h"
#include <utils/vk_descriptors.h>
#include <graphics/Renderer.h>
#include <core/cvars.h>
#include <chrono>

static AutoCVar_Int cvCullMode("cull.frustum", "Toggle frustum culling", 1, CVarFlags::EditCheckBox);

void FrustumCullPass::init(VulkanCore* vulkanCore, ResourceManager* resourceManager) {
	_vulkanCore = vulkanCore;

	auto cullShader = resourceManager->load<Shader>("frustumcull.comp", VK_SHADER_STAGE_COMPUTE_BIT, "main");

	ComputePipelineDesc pipelineDesc{
		.computeShader = cullShader,
	};

	_pipeline = ComputePipeline(_vulkanCore->get_device(), resourceManager, pipelineDesc);
}

void FrustumCullPass::cleanup() {
	_pipeline.cleanup();
	_vulkanCore = nullptr;
}

void FrustumCullPass::dispatch(RenderContext& ctx, FrameData& frameData, DrawContext& drawCtx, const GPUSceneData& sceneData, VkExtent2D drawExtent, const FrustumCullPassParameters& params, const RenderGraph& graph)
{
	if (drawCtx.opaqueObjects.empty()) return;

	// statistics
	{
		// read cull stats from previous frame
		auto* statsRead = static_cast<CullStatsData*>(frameData._cullStatsBuffer->get_mapped_data());
		uint32_t visibleDraws = statsRead->visibleDraws;
		uint32_t visibleTriangles = statsRead->visibleTriangles;

		// reset stats buffer for next frame
		statsRead->visibleDraws = 0;
		statsRead->visibleTriangles = 0;

		// calculate culled meshes
		const uint32_t totalMeshes = static_cast<uint32_t>(drawCtx.opaqueObjects.size());
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
	const uint32_t drawCount = static_cast<uint32_t>(drawCtx.opaqueObjects.size());
	const uint32_t batchCount = static_cast<uint32_t>(drawCtx.opaqueBatches.size());
	drawCtx.indirectDrawCount = drawCount;

	// allocate GPU buffers
	const size_t objectBufferSize = sizeof(GPUObjectData) * drawCount;
	const size_t instanceBufferSize = sizeof(uint32_t) * drawCount;
	const size_t indirectBufferSize = sizeof(VkDrawIndexedIndirectCommand) * drawCount;
	const size_t countBufferSize = sizeof(uint32_t) * batchCount;

	frameData._gpuObjectBuffer->resize(objectBufferSize);
	frameData._gpuInstanceBuffer->resize(instanceBufferSize);
	frameData._gpuIndirectBuffer->resize(indirectBufferSize);
	frameData._gpuCountBuffer->resize(countBufferSize);

	memset(frameData._gpuCountBuffer->get_mapped_data(), 0, sizeof(uint32_t) * batchCount);

	// get mapped pointers to the buffers
	auto* mappedObjects = static_cast<GPUObjectData*>(frameData._gpuObjectBuffer->get_mapped_data());
	auto* mappedCommands = static_cast<VkDrawIndexedIndirectCommand*>(frameData._gpuIndirectBuffer->get_mapped_data());

	// fill object and indirect command buffers with data
	for (uint32_t b = 0; b < batchCount; b++) {
		const auto& batch = drawCtx.opaqueBatches[b];
		const uint32_t batchEnd = batch.first + batch.count;

		for (uint32_t i = batch.first; i < batchEnd; i++) {
			const RenderObject& obj = drawCtx.opaqueObjects[i];

			mappedObjects[i] = GPUObjectData{
				.worldMatrix = obj.transform,
				.sphereBounds = glm::vec4(obj.bounds.origin, obj.bounds.sphereRadius),
				.vertexBufferAddress = obj.vertexBufferAddress,
				.batchIndex = b,
				.batchOffset = batch.first,
				.materialIndex = obj.materialIndex
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

	VkDescriptorSetLayout cullSetLayout = _pipeline.get_descriptor_set_layout(0);
	VkDescriptorSet cullSet = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), cullSetLayout);

	// update descriptor set with the new buffers
	DescriptorWriter writer;
	writer.write_buffer(0, frameData._gpuObjectBuffer->get_buffer(), sizeof(GPUObjectData) * drawCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.write_buffer(1, frameData._gpuInstanceBuffer->get_buffer(), sizeof(uint32_t) * drawCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.write_buffer(2, frameData._gpuIndirectBuffer->get_buffer(), sizeof(VkDrawIndexedIndirectCommand) * drawCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.write_buffer(3, frameData._cullStatsBuffer->get_buffer(), sizeof(CullStatsData), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.write_buffer(4, frameData._gpuCountBuffer->get_buffer(), sizeof(uint32_t) * batchCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.update_set(_vulkanCore->get_device(), cullSet);

	ctx.bind_pipeline(_pipeline);
	ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline.get_layout(), 0, cullSet);

	// set push constants
	CullPushConstants cullPush{};
	get_frustum_planes(sceneData.viewproj, cullPush.frustumPlanes);
	cullPush.drawCount = drawCount;
	cullPush.cullMode = static_cast<uint32_t>(cvCullMode.Get());
	ctx.set_push_constants(_pipeline.get_layout(), VK_SHADER_STAGE_COMPUTE_BIT, cullPush);
	ctx.dispatch((drawCount + 255) / 256);

	auto end = std::chrono::system_clock::now();
	_stats.passTimeMs = std::chrono::duration<float, std::milli>(end - start).count();
}
