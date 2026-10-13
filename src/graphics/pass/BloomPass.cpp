#include "BloomPass.h"

#include <graphics/reflection/DescriptorBinder.h>
#include <graphics/reflection/PushConstantDispatcher.h>
#include <graphics/Renderer.h>
#include <resources/DefaultResources.h>

void BloomPass::init(VulkanCore* vulkanCore, ResourceManager* resourceManager, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat)
{
	_vulkanCore = vulkanCore;

	auto vert = resourceManager->load<Shader>("deferred_lighting.vert", VK_SHADER_STAGE_VERTEX_BIT, "main");
	auto frag = resourceManager->load<Shader>("bloom_bright_extract.frag", VK_SHADER_STAGE_FRAGMENT_BIT, "main");
	auto blurFrag = resourceManager->load<Shader>("gaussian_blur.frag", VK_SHADER_STAGE_FRAGMENT_BIT, "main");

	GraphicsPipelineDesc extractDesc{
		.shaders = {vert, frag},
		.colorAttachmentFormats = {colorFormat},
		.depthAttachmentFormat = VK_FORMAT_UNDEFINED,
		.depthState = {.depthTest = false, .depthWrite = false},
		.rasterState = {.cullMode = VK_CULL_MODE_NONE, .polygonMode = VK_POLYGON_MODE_FILL}
	};
	_brightExtractPipeline = GraphicsPipeline(_vulkanCore->get_device(), resourceManager, extractDesc);

	GraphicsPipelineDesc blurDesc{
		.shaders = {vert, blurFrag},
		.colorAttachmentFormats = {colorFormat},
		.depthAttachmentFormat = VK_FORMAT_UNDEFINED,
		.depthState = {.depthTest = false, .depthWrite = false},
		.rasterState = {.cullMode = VK_CULL_MODE_NONE, .polygonMode = VK_POLYGON_MODE_FILL}
	};
	_blurPipeline = GraphicsPipeline(_vulkanCore->get_device(), resourceManager, blurDesc);
}

void BloomPass::cleanup()
{
	_brightExtractPipeline.cleanup();
	_blurPipeline.cleanup();
	_vulkanCore = nullptr;
}

void BloomPass::render_extract(RenderContext & ctx, FrameData & frameData, const BloomBrightExtractPassParameters & params, const RenderGraph & graph)
{
	VkDescriptorSet extractSet = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _brightExtractPipeline.get_descriptor_set_layout(0));

	std::unordered_map<std::string, VkDescriptorImageInfo> images = {
		{ "sceneColor", { DefaultResources::get().defaultSamplerLinear->get_sampler(), graph.get_image_view(params.sceneColor), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}
	};
	DescriptorBinder::update_set(_vulkanCore->get_device(), extractSet, _brightExtractPipeline.get_reflection().sets.at(0), {}, images);

	ctx.bind_pipeline(_brightExtractPipeline);
	ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _brightExtractPipeline.get_layout(), 0, extractSet);
	ctx.set_cull_mode(VK_CULL_MODE_NONE);

	// draw fullscreen triangle
	ctx.draw(3, 1, 0, 0);
}

void BloomPass::render_blur(RenderContext& ctx, FrameData& frameData, const GaussianBlurPassParameters& params, const RenderGraph& graph)
{
	VkDescriptorSet blurSet = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _blurPipeline.get_descriptor_set_layout(0));

	std::unordered_map<std::string, VkDescriptorImageInfo> images = {
		{ "image", { DefaultResources::get().defaultSamplerLinear->get_sampler(), graph.get_image_view(params.inputColor), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}
	};
	DescriptorBinder::update_set(_vulkanCore->get_device(), blurSet, _blurPipeline.get_reflection().sets.at(0), {}, images);

	ctx.bind_pipeline(_blurPipeline);
	ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _blurPipeline.get_layout(), 0, blurSet);

	PushConstantDispatcher pc(_blurPipeline.get_reflection());
	pc.set("horizontal", params.horizontal);
	pc.flush(ctx.get_cmd(), _blurPipeline.get_layout());

	ctx.set_cull_mode(VK_CULL_MODE_NONE);

	// draw fullscreen triangle
	ctx.draw(3, 1, 0, 0);
}
