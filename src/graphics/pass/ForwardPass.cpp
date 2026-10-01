#pragma once

#include "ForwardPass.h"

#include <utils/vk_descriptors.h>
#include <graphics/Renderer.h>
#include <graphics/utils/vk_pipelines.h>
#include <graphics/utils/vk_utils.h>
#include <graphics/utils/vk_initializers.h>
#include <algorithm>
#include <core/cvars.h>
#include <chrono>

static AutoCVar_Int cvCullMode("r.cullMode", "Face culling mode: 0 = None, 1 = Back, 2 = Front", 1);
static AutoCVar_Int cvFrontFace("r.frontFaceCW", "Front face winding: 1 = Clockwise, 0 = Counter-Clockwise", 1, CVarFlags::EditCheckBox);

void ForwardPass::init(VulkanCore* vulkanCore, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat)
{
	_vulkanCore = vulkanCore;
	_materialDescriptorLayout = materialLayout;

	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); // binding 0: uniform buffer for scene data
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // binding 1: storage buffer for object data
	builder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // binding 2: storage buffer for instance data
	builder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 3: combined image sampler for shadow map
	builder.add_binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 4: combined image sampler for cubemap
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

void ForwardPass::render(VkCommandBuffer cmd, FrameData & frameData, RenderResourceManager& resources, DrawContext & ctx, const GPUSceneData & sceneData, VkExtent2D drawExtent)
{	
	if (ctx.indirectDrawCount == 0 || frameData._gpuIndirectBuffer.buffer == VK_NULL_HANDLE || ctx.opaqueBatches.empty()) {
		return;
	}

	// begin clock
	auto start = std::chrono::system_clock::now();

	// transition resources to the correct states for rendering
	resources.transition(cmd, "ShadowMap", ResourceState::ShaderReadOnly);
	resources.transition(cmd, "SkyCubemap", ResourceState::ShaderReadOnly);
	resources.transition(cmd, "SceneColor", ResourceState::ColorAttachment);
	resources.transition(cmd, "SceneDepth", ResourceState::DepthAttachment);

	VkClearValue clearColor{ 0.1f, 0.1f, 0.1f, 1.0f };
	VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(resources.get_view("SceneColor"), &clearColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(resources.get_view("SceneDepth"), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
	VkRenderingInfo renderInfo = vkinit::rendering_info(drawExtent, &colorAttachment, &depthAttachment);

	vkCmdBeginRendering(cmd, &renderInfo);

	// create a descriptor set that binds the scene data buffer and update it
	VkDescriptorSet globalDescriptor = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _sceneDataDescriptorLayout);

	DescriptorWriter writer;
	writer.write_buffer(0, frameData.sceneDataBuffer.buffer, sizeof(GPUSceneData), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	writer.write_buffer(1, frameData._gpuObjectBuffer.buffer, sizeof(GPUObjectData) * ctx.indirectDrawCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.write_buffer(2, frameData._gpuInstanceBuffer.buffer, sizeof(uint32_t) * ctx.indirectDrawCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.write_image(3, resources.get_view("ShadowMap"), _vulkanCore->_shadowSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.write_image(4, resources.get_view("SkyCubemap"), _vulkanCore->_defaultSamplerLinear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.update_set(_vulkanCore->get_device(), globalDescriptor);

	// set dynamic viewport and scissor
	VkViewport viewport{0.f, 0.f, static_cast<float>(drawExtent.width), static_cast<float>(drawExtent.height), 0.f, 1.f};
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	VkRect2D scissor{{0, 0}, drawExtent};
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	// set culling mode and front face winding based on cvars
	VkCullModeFlags cullFlags = VK_CULL_MODE_NONE;
	int mode = cvCullMode.Get();
	if (mode == 1) 
		cullFlags = VK_CULL_MODE_BACK_BIT;
	else if (mode == 2) 
		cullFlags = VK_CULL_MODE_FRONT_BIT;
	vkCmdSetCullMode(cmd, cullFlags);
	vkCmdSetFrontFace(cmd, cvFrontFace.Get() != 0 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);

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
	
	vkCmdEndRendering(cmd);

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
		return;
	}
	VkShaderModule meshVertexShader;
	if (!vkutil::load_shader_module("../../shaders/mesh.vert.spv", device, &meshVertexShader)) {
		fmt::println("Error when building the mesh vertex shader module");
		return;
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
