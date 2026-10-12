#include "DeferredLightingPass.h"

#include <graphics/reflection/DescriptorBinder.h>
#include <graphics/reflection/PushConstantDispatcher.h>
#include <graphics/Renderer.h>
#include <core/cvars.h>
#include <chrono>
#include <resources/DefaultResources.h>

extern AutoCVar_Int cvSSAO;

void DeferredLightingPass::init(VulkanCore* vulkanCore, ResourceManager* resourceManager, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat)
{
	_vulkanCore = vulkanCore;

	auto vert = resourceManager->load<Shader>("deferred_lighting.vert", VK_SHADER_STAGE_VERTEX_BIT, "main");
	auto frag = resourceManager->load<Shader>("deferred_lighting.frag", VK_SHADER_STAGE_FRAGMENT_BIT, "main");

	GraphicsPipelineDesc desc{
		.shaders = { vert, frag },
		.colorAttachmentFormats = { colorFormat },
		.depthAttachmentFormat = VK_FORMAT_UNDEFINED, // no depth attachment for fullscreen pass
		.depthState = {.depthTest = false, .depthWrite = false },
		.rasterState = {.cullMode = VK_CULL_MODE_NONE, .polygonMode = VK_POLYGON_MODE_FILL }
	};
	_pipeline = GraphicsPipeline(_vulkanCore->get_device(), resourceManager, desc);
}

void DeferredLightingPass::cleanup()
{
	_pipeline.cleanup();
	_vulkanCore = nullptr;
}

void DeferredLightingPass::render(RenderContext& ctx, FrameData& frameData, DrawContext& drawCtx, const GPUSceneData& sceneData, const DeferredPassParameters& params, const RenderGraph& graph)
{
	const auto& env = drawCtx.environment;

	// update descriptor set
	VkDescriptorSet lightingSet = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _pipeline.get_descriptor_set_layout(0));

	std::unordered_map<std::string, VkDescriptorBufferInfo> buffers = {
		{"sceneData", { frameData.sceneDataBuffer->get_buffer(), 0, sizeof(GPUSceneData) }} 
	};

	std::unordered_map<std::string, VkDescriptorImageInfo> images = {
		{"gPosition", { DefaultResources::get().defaultSamplerNearest->get_sampler(), graph.get_image_view(params.gPosition), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }},
		{"gNormal", { DefaultResources::get().defaultSamplerNearest->get_sampler(), graph.get_image_view(params.gNormal), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }},
		{"gAlbedo", { DefaultResources::get().defaultSamplerNearest->get_sampler(), graph.get_image_view(params.gAlbedo), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }},
		{"gMetalRough", { DefaultResources::get().defaultSamplerNearest->get_sampler(), graph.get_image_view(params.gMetalRough), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }},
		{"shadowMap", { DefaultResources::get().shadowSampler->get_sampler(), graph.get_image_view(params.shadowMap), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }},
		{"irradianceMap", { env.cubemapSampler, env.irradianceView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }},
		{"prefilterMap", { env.cubemapSampler, env.prefilterView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }},
		{"brdfLUT", { env.lutSampler, env.brdfLutView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }},
		{"pointShadowMap", { env.cubemapSampler, graph.get_image_view(params.pointShadowMap), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }},
		{"ssaoMap", { DefaultResources::get().defaultSamplerLinear->get_sampler(), graph.get_image_view(params.ssao), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }},
		{"gEmissive", { DefaultResources::get().defaultSamplerNearest->get_sampler(), graph.get_image_view(params.gEmissive), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }}
	};

	DescriptorBinder::update_set(_vulkanCore->get_device(), lightingSet, _pipeline.get_reflection().sets.at(0), buffers, images);

	ctx.bind_pipeline(_pipeline);
	ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.get_layout(), 0, lightingSet);

	PushConstantDispatcher pc(_pipeline.get_reflection());
	pc.set("enableSSAO", static_cast<int>(cvSSAO.Get()));
	pc.flush(ctx.get_cmd(), _pipeline.get_layout());

	ctx.set_cull_mode(VK_CULL_MODE_NONE);
	ctx.draw(3, 1, 0, 0); // draw fullscreen triangle
}
