#include "ShadowPass.h"
#include <utils/vk_descriptors.h>
#include <utils/vk_initializers.h>
#include <utils/vk_pipelines.h>
#include <utils/vk_utils.h>
#include <graphics/Renderer.h>
#include <chrono>

void ShadowPass::init(VulkanCore* vulkanCore, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat)
{
	_vulkanCore = vulkanCore;
	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // binding 0: storage buffer for scene data
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // binding 1: storage buffer for instance index
	_descriptorLayout = builder.build(_vulkanCore->get_device(), VK_SHADER_STAGE_VERTEX_BIT);

	VkPushConstantRange pushRange{
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
		.offset = 0,
		.size = sizeof(glm::mat4)
	};

	// create pipeline layout
	VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipeline_layout_create_info();
	layoutInfo.setLayoutCount = 1;
	layoutInfo.pSetLayouts = &_descriptorLayout;
	layoutInfo.pushConstantRangeCount = 1;
	layoutInfo.pPushConstantRanges = &pushRange;
	VK_CHECK(vkCreatePipelineLayout(_vulkanCore->get_device(), &layoutInfo, nullptr, &_pipeline.layout));

	// load shader
	VkShaderModule vertShader;
	if (!vkutil::load_shader_module("../../shaders/shadow.vert.spv", _vulkanCore->get_device(), &vertShader)) {
		fmt::println("Error when building the shadow vertex shader module");
		return;
	}
	
	// create the pipeline
	PipelineBuilder pipelineBuilder;
	pipelineBuilder._shaderStages.push_back(vkinit::pipeline_shader_stage_create_info(VK_SHADER_STAGE_VERTEX_BIT, vertShader));
	pipelineBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	pipelineBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	pipelineBuilder.set_cull_mode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_CLOCKWISE);
	pipelineBuilder.set_multisampling_none();
	pipelineBuilder.disable_blending();
	pipelineBuilder.enable_depthtest(true, VK_COMPARE_OP_LESS_OR_EQUAL);
	pipelineBuilder.set_depth_format(depthFormat);
	pipelineBuilder._renderInfo.colorAttachmentCount = 0;
	pipelineBuilder._renderInfo.pColorAttachmentFormats = nullptr;
	pipelineBuilder._pipelineLayout = _pipeline.layout;
	_pipeline.pipeline = pipelineBuilder.build_pipeline(_vulkanCore->get_device());

	// cleanup shader module
	vkDestroyShaderModule(_vulkanCore->get_device(), vertShader, nullptr);
}

void ShadowPass::cleanup()
{
	VkDevice dev = _vulkanCore->get_device();
	vkDestroyPipeline(dev, _pipeline.pipeline, nullptr);
	vkDestroyPipelineLayout(dev, _pipeline.layout, nullptr);
	vkDestroyDescriptorSetLayout(dev, _descriptorLayout, nullptr);
	_vulkanCore = nullptr;
}

void ShadowPass::render(VkCommandBuffer cmd, FrameData & frameData, RenderResourceManager & resources, DrawContext & ctx, const GPUSceneData & sceneData, VkExtent2D drawExtent)
{
	if (ctx.indirectDrawCount == 0 || frameData._gpuIndirectBuffer.buffer == VK_NULL_HANDLE || ctx.opaqueBatches.empty()) {
		return;
	}

	// begin clock
	auto start = std::chrono::system_clock::now();

	// transition shadow map to depth attachment
	resources.transition(cmd, "ShadowMap", ResourceState::DepthAttachment);
	 
	// setup rendering info
	VkExtent2D shadowExtent = resources.get_extent("ShadowMap");
	VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(resources.get_view("ShadowMap"), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
	depthAttachment.clearValue.depthStencil.depth = 1.0f;
	VkRenderingInfo renderInfo = vkinit::rendering_info(shadowExtent, nullptr, &depthAttachment);

	vkCmdBeginRendering(cmd, &renderInfo);

	// set dynamic viewport and scissor
	VkViewport viewport{ 0.f, 0.f, static_cast<float>(drawExtent.width), static_cast<float>(drawExtent.height), 0.f, 1.f };
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	VkRect2D scissor{ {0, 0}, drawExtent };
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	vkCmdSetCullMode(cmd, VK_CULL_MODE_BACK_BIT);
	vkCmdSetFrontFace(cmd, VK_FRONT_FACE_CLOCKWISE);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.pipeline);
	vkCmdPushConstants(cmd, _pipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &sceneData.sunlightViewproj);

	// create a descriptor set that binds the scene data buffer and update it
	VkDescriptorSet shadowDescriptor = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _descriptorLayout);
	DescriptorWriter writer;
	writer.write_buffer(0, frameData._gpuObjectBuffer.buffer, sizeof(GPUObjectData) * ctx.indirectDrawCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.write_buffer(1, frameData._gpuInstanceBuffer.buffer, sizeof(uint32_t) * ctx.indirectDrawCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.update_set(_vulkanCore->get_device(), shadowDescriptor);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.layout, 0, 1, &shadowDescriptor, 0, nullptr);

	VkBuffer lastIndexBuffer = VK_NULL_HANDLE;
	for (const IndirectBatch& batch : ctx.opaqueBatches) {
		// rebind index buffer if needed
		if (batch.indexBuffer != lastIndexBuffer) {
			lastIndexBuffer = batch.indexBuffer;
			vkCmdBindIndexBuffer(cmd, batch.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
		}

		// draw batch
		VkDeviceSize indirectOffset = batch.first * sizeof(VkDrawIndexedIndirectCommand);
		VkDeviceSize countOffset = batch.batchIndex * sizeof(uint32_t);

		vkCmdDrawIndexedIndirectCount(cmd, frameData._gpuIndirectBuffer.buffer, indirectOffset, frameData._gpuCountBuffer.buffer, countOffset, batch.count, sizeof(VkDrawIndexedIndirectCommand));
	}
	
	vkCmdEndRendering(cmd);

	// transition shadow map to shader read only for sampling in forward pass
	resources.transition(cmd, "ShadowMap", ResourceState::ShaderReadOnly);

	auto end = std::chrono::system_clock::now();
	_stats.clear();
	_stats.name = "Shadow Pass";
	_stats.passTimeMs = std::chrono::duration<float, std::milli>(end - start).count();
}
