#include "PostProcessPass.h"

#include <graphics/reflection/DescriptorBinder.h>
#include <graphics/reflection/PushConstantDispatcher.h>
#include <graphics/Renderer.h>
#include <resources/DefaultResources.h>
#include <core/cvars.h>

AutoCVar_Float cvExposure("r.exposure", "Camera exposure multiplier", 1.0f, CVarFlags::EditFloatSlider, 0.1f, 10.0f);

void PostProcessPass::init(VulkanCore* vulkanCore, ResourceManager* resourceManager, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat)
{
	_vulkanCore = vulkanCore;
	auto vert = resourceManager->load<Shader>("deferred_lighting.vert", VK_SHADER_STAGE_VERTEX_BIT, "main");
	auto frag = resourceManager->load<Shader>("post_process.frag", VK_SHADER_STAGE_FRAGMENT_BIT, "main");

	GraphicsPipelineDesc desc{
		.shaders = { vert, frag},
		.colorAttachmentFormats = { colorFormat },
		.depthAttachmentFormat = VK_FORMAT_UNDEFINED,
		.depthState = {.depthTest = false, .depthWrite = false },
		.rasterState = {.cullMode = VK_CULL_MODE_NONE, .polygonMode = VK_POLYGON_MODE_FILL }
	};
	_pipeline = GraphicsPipeline(_vulkanCore->get_device(), resourceManager, desc);
}

void PostProcessPass::cleanup()
{
	_pipeline.cleanup();
	_vulkanCore = nullptr;
}

void PostProcessPass::render(RenderContext& ctx, FrameData& frameData, const PostProcessPassParameters& params, const RenderGraph& graph)
{
	VkDescriptorSet postProcessSet = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _pipeline.get_descriptor_set_layout(0));

	std::unordered_map<std::string, VkDescriptorImageInfo> images = {
		{"sceneColor", { DefaultResources::get().defaultSamplerLinear->get_sampler(), graph.get_image_view(params.sceneColor), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }},
		{"bloomColor", { DefaultResources::get().defaultSamplerLinear->get_sampler(), graph.get_image_view(params.bloomColor), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}

	};

	DescriptorBinder::update_set(_vulkanCore->get_device(), postProcessSet, _pipeline.get_reflection().sets.at(0), {}, images);

	ctx.bind_pipeline(_pipeline);
	ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.get_layout(), 0, postProcessSet);

	// set push constants
	PushConstantDispatcher pc(_pipeline.get_reflection());
	pc.set("exposure", static_cast<float>(cvExposure.Get()));
	pc.flush(ctx.get_cmd(), _pipeline.get_layout());

	// draw fullscreen triangle
	ctx.draw(3, 1, 0, 0);
}
