#include "DeferredLightingPass.h"

#include <graphics/reflection/DescriptorBinder.h>
#include <graphics/reflection/PushConstantDispatcher.h>
#include <graphics/Renderer.h>
#include <core/cvars.h>
#include <chrono>
#include <resources/DefaultResources.h>

extern AutoCVar_Int cvSSAO;

void DeferredLightingPass::init(VulkanCore* vulkanCore, ResourceManager* resourceManager)
{
	_vulkanCore = vulkanCore;

	auto vert = resourceManager->load<Shader>("deferred_lighting.vert", VK_SHADER_STAGE_VERTEX_BIT, "main");
	auto frag = resourceManager->load<Shader>("deferred_lighting.frag", VK_SHADER_STAGE_FRAGMENT_BIT, "main");

	GraphicsPipelineDesc desc{
		.shaders = { vert, frag },
		.colorAttachmentFormats = { VK_FORMAT_R16G16B16A16_SFLOAT },
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

void DeferredLightingPass::add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view)
{
	auto& targets = blackboard.get<SceneRenderContext>();

	RGTextureDesc colorDesc{
			.extent = { view.drawExtent.width, view.drawExtent.height, 1 },
			.format = VK_FORMAT_R16G16B16A16_SFLOAT,
			.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
	};
	targets.sceneColor = graph.create_texture("SceneColor", colorDesc);

	graph.add_pass<DeferredLightingGraphParams>("DeferredLightingPass",
		[&](RGPassBuilder& builder, DeferredLightingGraphParams& params) {
			params.gPosition = builder.read_texture(targets.gPosition, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			params.gNormal = builder.read_texture(targets.gNormal, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			params.gAlbedo = builder.read_texture(targets.gAlbedo, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			params.gMetalRough = builder.read_texture(targets.gMetalRough, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			params.gEmissive = builder.read_texture(targets.gEmissive, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			params.shadowMap = builder.read_texture(targets.dirShadowMap, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			params.pointShadowMap = builder.read_texture(targets.pointShadowMap, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			params.ssao = builder.read_texture(targets.ssaoTexture, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			params.irradianceMap = builder.read_texture(targets.irradianceMap, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			params.prefilterMap = builder.read_texture(targets.prefilterMap, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			params.brdfLut = builder.read_texture(targets.brdfLut, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

			params.sceneColor = builder.set_color_attachment(targets.sceneColor, VK_ATTACHMENT_LOAD_OP_CLEAR, { 0.0f, 0.0f, 0.0f, 1.0f });
			params.ssaoEnabled = (targets.ssaoTexture == targets.defaultWhite) ? 0 : 1;
			builder.set_extent(view.drawExtent);
		},
		[this, &view, &graph](const DeferredLightingGraphParams& params, RenderContext& ctx) {
			auto& defaults = DefaultResources::get();
			VkSampler nearest = defaults.defaultSamplerNearest->get_sampler();
			VkSampler linear = defaults.defaultSamplerLinear->get_sampler();
			VkSampler cube = defaults.cubemapSampler->get_sampler();
			VkSampler lut = defaults.lutSampler->get_sampler();
			VkSampler shadow = defaults.shadowSampler->get_sampler();

			VkDescriptorSet lightingSet = view.frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _pipeline.get_descriptor_set_layout(0));

			std::unordered_map<std::string, VkDescriptorBufferInfo> buffers = {
				{"sceneData", { view.frameData.sceneDataBuffer->get_buffer(), 0, sizeof(GPUSceneData) }}
			};

			std::unordered_map<std::string, VkDescriptorImageInfo> images = {
				{ "gPosition", { nearest, graph.get_image_view(params.gPosition), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }},
				{ "gNormal", { nearest, graph.get_image_view(params.gNormal), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }},
				{ "gAlbedo", { nearest, graph.get_image_view(params.gAlbedo), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }},
				{ "gMetalRough", { nearest, graph.get_image_view(params.gMetalRough), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }},
				{ "gEmissive", { nearest, graph.get_image_view(params.gEmissive), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }},
				{ "shadowMap", { shadow,  graph.get_image_view(params.shadowMap), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }},
				{ "pointShadowMap", { cube, graph.get_image_view(params.pointShadowMap), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }},
				{ "ssaoMap", { linear,  graph.get_image_view(params.ssao), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }},
				{ "irradianceMap", { cube, graph.get_image_view(params.irradianceMap), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }},
				{ "prefilterMap", { cube, graph.get_image_view(params.prefilterMap), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }},
				{ "brdfLUT", { lut, graph.get_image_view(params.brdfLut), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }}
			};

			DescriptorBinder::update_set(_vulkanCore->get_device(), lightingSet, _pipeline.get_reflection().sets.at(0), buffers, images);

			ctx.bind_pipeline(_pipeline);
			ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.get_layout(), 0, lightingSet);

			PushConstantDispatcher pc(_pipeline.get_reflection());
			pc.set("enableSSAO", params.ssaoEnabled);
			pc.flush(ctx.get_cmd(), _pipeline.get_layout());

			ctx.set_cull_mode(VK_CULL_MODE_NONE);
			ctx.draw(3, 1, 0, 0); // draw fullscreen triangle
		});
}
