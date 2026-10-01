#include "SkyboxPass.h"
#include <utils/vk_descriptors.h>
#include <utils/vk_initializers.h>
#include <utils/vk_pipelines.h>
#include <utils/vk_utils.h>
#include <graphics/Renderer.h>
#include <chrono>

void SkyboxPass::init(VulkanCore* vulkanCore, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat)
{
	_vulkanCore = vulkanCore;

	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); // binding 0: uniform buffer for scene data
	builder.add_binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 4: combined image sampler for skybox texture
	_descriptorLayout = builder.build(_vulkanCore->get_device(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

	// create pipeline layout
	VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipeline_layout_create_info();
	layoutInfo.setLayoutCount = 1;
	layoutInfo.pSetLayouts = &_descriptorLayout;
	VK_CHECK(vkCreatePipelineLayout(_vulkanCore->get_device(), &layoutInfo, nullptr, &_pipeline.layout));

	// load shader
	VkShaderModule vertShader, fragShader;
	if (!vkutil::load_shader_module("../../shaders/skybox.vert.spv", _vulkanCore->get_device(), &vertShader)) {
		fmt::println("Error when building the skybox vertex shader module");
		return;
	}
	if (!vkutil::load_shader_module("../../shaders/skybox.frag.spv", _vulkanCore->get_device(), &fragShader)) {
		fmt::println("Error when building the skybox fragment shader module");
		return;
	}

	// create the pipeline
	PipelineBuilder pipelineBuilder;
	pipelineBuilder.set_shaders(vertShader, fragShader);
	pipelineBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	pipelineBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	pipelineBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	pipelineBuilder.set_multisampling_none();
	pipelineBuilder.disable_blending();
	// test GREATER_OR_EQUAL to draw only on empty pixels without writing depth
	pipelineBuilder.enable_depthtest(false, VK_COMPARE_OP_GREATER_OR_EQUAL);
	pipelineBuilder.set_color_attachment_format(colorFormat);
	pipelineBuilder.set_depth_format(depthFormat);
	pipelineBuilder._pipelineLayout = _pipeline.layout;
	_pipeline.pipeline = pipelineBuilder.build_pipeline(_vulkanCore->get_device());

	// cleanup shader modules
	vkDestroyShaderModule(_vulkanCore->get_device(), vertShader, nullptr);
	vkDestroyShaderModule(_vulkanCore->get_device(), fragShader, nullptr);
}

void SkyboxPass::cleanup()
{
	VkDevice dev = _vulkanCore->get_device();
	vkDestroyPipeline(dev, _pipeline.pipeline, nullptr);
	vkDestroyPipelineLayout(dev, _pipeline.layout, nullptr);
	vkDestroyDescriptorSetLayout(dev, _descriptorLayout, nullptr);
	_vulkanCore = nullptr;
}

void SkyboxPass::render(VkCommandBuffer cmd, FrameData & frameData, RenderResourceManager & resources, DrawContext & ctx, const GPUSceneData & sceneData, VkExtent2D drawExtent)
{
	// begin clock
	auto start = std::chrono::system_clock::now();

	// transition color, depth, and skybox attachments to correct state
	resources.transition(cmd, "SceneColor", ResourceState::ColorAttachment);
	resources.transition(cmd, "SceneDepth", ResourceState::DepthAttachment);
	resources.transition(cmd, "SkyCubemap", ResourceState::ShaderReadOnly); 

	// setup rendering info
	VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(resources.get_view("SceneColor"), nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; 
	VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(resources.get_view("SceneDepth"), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
	depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	VkRenderingInfo renderInfo = vkinit::rendering_info(drawExtent, &colorAttachment, &depthAttachment);

	vkCmdBeginRendering(cmd, &renderInfo);

	// set dynamic viewport and scissor
	VkViewport viewport{ 0.f, 0.f, static_cast<float>(drawExtent.width), static_cast<float>(drawExtent.height), 0.f, 1.f };
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	VkRect2D scissor{ {0, 0}, drawExtent };
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
	vkCmdSetFrontFace(cmd, VK_FRONT_FACE_CLOCKWISE);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.pipeline);

	// bind descriptor sets
	VkDescriptorSet skyboxSet = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _descriptorLayout);
	DescriptorWriter writer;
	writer.write_buffer(0, frameData.sceneDataBuffer.buffer, sizeof(GPUSceneData), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	writer.write_image(4, resources.get_view("SkyCubemap"), _vulkanCore->_defaultSamplerLinear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.update_set(_vulkanCore->get_device(), skyboxSet);

	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.layout, 0, 1, &skyboxSet, 0, nullptr);
	vkCmdDraw(cmd, 3, 1, 0, 0);

	vkCmdEndRendering(cmd);

	auto end = std::chrono::system_clock::now();
	_stats.clear();
	_stats.name = "Skybox Pass";
	_stats.passTimeMs = std::chrono::duration<float, std::milli>(end - start).count();
}
