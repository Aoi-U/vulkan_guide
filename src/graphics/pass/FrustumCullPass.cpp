#include "FrustumCullPass.h"
#include <utils/vk_descriptors.h>
#include <graphics/Renderer.h>
#include <core/cvars.h>
#include <chrono>
#include <imgui.h>

static AutoCVar_Int cvCullMode("cull.frustum", "Toggle frustum culling", 1, CVarFlags::EditCheckBox);

void FrustumCullPass::init(VulkanCore* vulkanCore, ResourceManager* resourceManager) {
	_vulkanCore = vulkanCore;

	auto cullShader = resourceManager->load<Shader>("frustumcull.comp", VK_SHADER_STAGE_COMPUTE_BIT, "main");

	ComputePipelineDesc pipelineDesc{ .computeShader = cullShader };
	_pipeline = ComputePipeline(_vulkanCore->get_device(), resourceManager, pipelineDesc);
}

void FrustumCullPass::cleanup() {
	_pipeline.cleanup();
	_vulkanCore = nullptr;
}

void FrustumCullPass::add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view)
{
	auto& targets = blackboard.get<SceneRenderContext>();

	graph.add_pass<FrustumCullPassParameters>("FrustumCull",
		[&](RGPassBuilder& builder, FrustumCullPassParameters& params) {
			params.gpuObjectBuffer = builder.read_buffer(targets.gpuObjectBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
			params.gpuInstanceBuffer = builder.write_buffer(targets.gpuInstanceBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
			params.gpuIndirectBuffer = builder.write_buffer(targets.gpuIndirectBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
			params.gpuCountBuffer = builder.write_buffer(targets.gpuCountBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
			params.cullStatsBuffer = builder.write_buffer(targets.cullStatsBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
			builder.set_side_effect(true);
		},
		[this, &view](const FrustumCullPassParameters&, RenderContext& ctx) {
			FrameData& frameData = view.frameData;
			DrawContext& drawCtx = view.drawCtx;
			if (drawCtx.opaqueObjects.empty()) return;

			// number of objects to draw
			const uint32_t opaqueCount = static_cast<uint32_t>(drawCtx.opaqueObjects.size());
			const uint32_t transCount = static_cast<uint32_t>(drawCtx.transparentObjects.size());
			const uint32_t totalObjects = std::max(1u, opaqueCount + transCount);
			const uint32_t batchCount = std::max(1u, static_cast<uint32_t>(drawCtx.opaqueBatches.size()));
			drawCtx.indirectDrawCount = opaqueCount;

			// allocate GPU buffers
			frameData._gpuObjectBuffer->resize(sizeof(GPUObjectData) * totalObjects);
			frameData._gpuInstanceBuffer->resize(sizeof(uint32_t) * totalObjects);
			frameData._gpuIndirectBuffer->resize(sizeof(VkDrawIndexedIndirectCommand) * opaqueCount);
			frameData._gpuCountBuffer->resize(sizeof(uint32_t) * batchCount);
			std::memset(frameData._gpuCountBuffer->get_mapped_data(), 0, sizeof(uint32_t) * batchCount);

			// get mapped pointers to the buffers
			auto* mappedObjects = static_cast<GPUObjectData*>(frameData._gpuObjectBuffer->get_mapped_data());
			auto* mappedCommands = static_cast<VkDrawIndexedIndirectCommand*>(frameData._gpuIndirectBuffer->get_mapped_data());

			// fill object and indirect command buffers with data
			for (uint32_t b = 0; b < static_cast<uint32_t>(drawCtx.opaqueBatches.size()); b++) {
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

			// update descriptor set with the new buffers
			VkDescriptorSet cullSet = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _pipeline.get_descriptor_set_layout(0));
			DescriptorWriter writer;
			writer.write_buffer(0, frameData._gpuObjectBuffer->get_buffer(), sizeof(GPUObjectData) * opaqueCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
			writer.write_buffer(1, frameData._gpuInstanceBuffer->get_buffer(), sizeof(uint32_t) * opaqueCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
			writer.write_buffer(2, frameData._gpuIndirectBuffer->get_buffer(), sizeof(VkDrawIndexedIndirectCommand) * opaqueCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
			writer.write_buffer(3, frameData._cullStatsBuffer->get_buffer(), sizeof(CullStatsData), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
			writer.write_buffer(4, frameData._gpuCountBuffer->get_buffer(), sizeof(uint32_t) * batchCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
			writer.update_set(_vulkanCore->get_device(), cullSet);

			ctx.bind_pipeline(_pipeline);
			ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline.get_layout(), 0, cullSet);

			// set push constants
			CullPushConstants cullPush{};
			get_frustum_planes(view.sceneData.viewproj, cullPush.frustumPlanes);
			cullPush.drawCount = opaqueCount;
			cullPush.cullMode = static_cast<uint32_t>(cvCullMode.Get());
			ctx.set_push_constants(_pipeline.get_layout(), VK_SHADER_STAGE_COMPUTE_BIT, cullPush);

			ctx.dispatch((opaqueCount + 255) / 256);
		});
}

void FrustumCullPass::on_imgui()
{
	bool cullEnabled = cvCullMode.Get() != 0;
	if (ImGui::Checkbox("GPU frustum culling", &cullEnabled)) {
		cvCullMode.Set(cullEnabled ? 1 : 0);
	}
}
