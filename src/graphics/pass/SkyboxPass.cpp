#include "SkyboxPass.h"
#include <utils/vk_descriptors.h>
#include <graphics/Renderer.h>
#include <chrono>

void SkyboxPass::init(VulkanCore* vulkanCore, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat)
{
	_vulkanCore = vulkanCore;

	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); // binding 0: uniform buffer for scene data
	builder.add_binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 4: combined image sampler for skybox texture
	_descriptorLayout = builder.build(_vulkanCore->get_device(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

	// load shaders
	auto vert = Shader::load(_vulkanCore->get_device(), VK_SHADER_STAGE_VERTEX_BIT, "../../shaders/skybox.vert.spv");
	auto frag = Shader::load(_vulkanCore->get_device(), VK_SHADER_STAGE_FRAGMENT_BIT, "../../shaders/skybox.frag.spv");

	// create pipeline
	GraphicsPipelineDesc pipelineDesc{
		.shaders = {vert, frag},
		.descriptorLayouts = {_descriptorLayout},
		.colorAttachmentFormats = {colorFormat},
		.depthAttachmentFormat = depthFormat,
		.depthState = {.depthTest = true, .depthWrite = false, .compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL}, // test GREATER_OR_EQUAL to draw only on empty pixels without writing depth
		.rasterState = {.cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_CLOCKWISE, .polygonMode = VK_POLYGON_MODE_FILL},
		.blendMode = BlendMode::None
	};
	_pipeline = GraphicsPipeline(_vulkanCore->get_device(), pipelineDesc);
}

void SkyboxPass::cleanup()
{
	VkDevice dev = _vulkanCore->get_device();
	_pipeline.cleanup();
	if (_descriptorLayout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(dev, _descriptorLayout, nullptr);
		_descriptorLayout = VK_NULL_HANDLE;
	}
	_vulkanCore = nullptr;
}

void SkyboxPass::render(RenderContext& ctx, FrameData& frameData, RenderResourceManager& resources, DrawContext& drawCtx, const GPUSceneData& sceneData)
{
	// begin clock
	auto start = std::chrono::system_clock::now();

	// bind descriptor sets
	VkDescriptorSet skyboxSet = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _descriptorLayout);
	DescriptorWriter writer;
	writer.write_buffer(0, frameData.sceneDataBuffer.buffer, sizeof(GPUSceneData), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	writer.write_image(4, resources.get_view("SkyCubemap"), _vulkanCore->_defaultSamplerLinear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.update_set(_vulkanCore->get_device(), skyboxSet);

	// bind pipeline and descriptor set
	ctx.bind_pipeline(_pipeline);
	ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.get_layout(), 0, skyboxSet);

	// draw full screen triangle
	ctx.draw(3, 1, 0, 0);

	auto end = std::chrono::system_clock::now();
	_stats.clear();
	_stats.name = "Skybox Pass";
	_stats.passTimeMs = std::chrono::duration<float, std::milli>(end - start).count();
}
