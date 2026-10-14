#include "BloomPass.h"

#include <graphics/reflection/DescriptorBinder.h>
#include <graphics/reflection/PushConstantDispatcher.h>
#include <graphics/Renderer.h>
#include <resources/DefaultResources.h>
#include <imgui.h>
#include <fmt/core.h>

extern AutoCVar_Int cvBloom;

void BloomPass::init(VulkanCore* vulkanCore, ResourceManager* resourceManager)
{
	_vulkanCore = vulkanCore;

	auto vert = resourceManager->load<Shader>("deferred_lighting.vert", VK_SHADER_STAGE_VERTEX_BIT, "main");
	auto frag = resourceManager->load<Shader>("bloom_bright_extract.frag", VK_SHADER_STAGE_FRAGMENT_BIT, "main");
	auto blurFrag = resourceManager->load<Shader>("gaussian_blur.frag", VK_SHADER_STAGE_FRAGMENT_BIT, "main");

	GraphicsPipelineDesc extractDesc{
		.shaders = {vert, frag},
		.colorAttachmentFormats = { VK_FORMAT_R16G16B16A16_SFLOAT },
		.depthAttachmentFormat = VK_FORMAT_UNDEFINED,
		.depthState = {.depthTest = false, .depthWrite = false},
		.rasterState = {.cullMode = VK_CULL_MODE_NONE, .polygonMode = VK_POLYGON_MODE_FILL}
	};
	_brightExtractPipeline = GraphicsPipeline(_vulkanCore->get_device(), resourceManager, extractDesc);

	GraphicsPipelineDesc blurDesc = extractDesc;
	blurDesc.shaders = { vert, blurFrag };
	_blurPipeline = GraphicsPipeline(_vulkanCore->get_device(), resourceManager, blurDesc);
}

void BloomPass::cleanup()
{
	_brightExtractPipeline.cleanup();
	_blurPipeline.cleanup();
	_vulkanCore = nullptr;
}

void BloomPass::add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view)
{
	if (cvBloom.Get() == 0) // bloom disabled
		return;

	auto& targets = blackboard.get<SceneRenderContext>();
	VkExtent2D bloomExtent = {
		std::max(1u, view.drawExtent.width / 2),
		std::max(1u, view.drawExtent.height / 2)
	};

	RGTextureDesc bloomDesc{
		.extent = { bloomExtent.width, bloomExtent.height, 1 },
		.format = VK_FORMAT_R16G16B16A16_SFLOAT,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
	};

	RGTextureHandle brightColor = graph.create_texture("BrightColor", bloomDesc);
	RGTextureHandle pingpongColor0 = graph.create_texture("PingPong0", bloomDesc);
	RGTextureHandle pingpongColor1 = graph.create_texture("PingPong1", bloomDesc);

	// pass 1: extract bright areas
	graph.add_pass<BloomBrightExtractPassParameters>("BloomBrightExtractPass",
		[&](RGPassBuilder& builder, BloomBrightExtractPassParameters& params) {
			params.sceneColor = builder.read_texture(targets.sceneColor, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			params.brightColor = builder.set_color_attachment(brightColor, VK_ATTACHMENT_LOAD_OP_CLEAR, { 0.0f, 0.0f, 0.0f, 1.0f });
			builder.set_extent(bloomExtent);
		},
		[this, &view, &graph](const BloomBrightExtractPassParameters& params, RenderContext& rc) {
			render_extract(view, graph, params, rc);
		});

	// pass 2: gaussian blur
	bool horizontal = true, firstIteration = true;
	RGTextureHandle currentBloomTarget = brightColor;

	for (int i = 0; i < _blurIterations; i++) {
		RGTextureHandle inputTexture = firstIteration ? brightColor : (horizontal ? pingpongColor1 : pingpongColor0); // first iteration uses brightColor, then alternate pingpong textures
		RGTextureHandle outputTexture = horizontal ? pingpongColor0 : pingpongColor1; // alternate pingpong textures for output

		graph.add_pass<GaussianBlurPassParameters>(fmt::format("GaussianBlurPass_{}", i),
			[&, inputTexture, outputTexture, horizontal](RGPassBuilder& builder, GaussianBlurPassParameters& params) {
				params.inputColor = builder.read_texture(inputTexture, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				params.blurColor = builder.set_color_attachment(outputTexture, VK_ATTACHMENT_LOAD_OP_CLEAR, { 0.0f, 0.0f, 0.0f, 1.0f });
				params.horizontal = horizontal ? 1 : 0;
				builder.set_extent(bloomExtent);
			},
			[this, &view, &graph](const GaussianBlurPassParameters& params, RenderContext& rc) {
				render_blur(view, graph, params, rc);
			});
		
		horizontal = !horizontal;
		firstIteration = false;
		currentBloomTarget = outputTexture;
	}

	targets.bloomTexture = currentBloomTarget;
}

void BloomPass::on_imgui()
{
	ImGui::SliderInt("Bloom blur iterations", &_blurIterations, 2, 16);
}

void BloomPass::render_extract(const RenderViewContext& view, const RenderGraph& graph, const BloomBrightExtractPassParameters& params, RenderContext& ctx)
{
	VkDescriptorSet extractSet = view.frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _brightExtractPipeline.get_descriptor_set_layout(0));

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

void BloomPass::render_blur(const RenderViewContext& view, const RenderGraph& graph, const GaussianBlurPassParameters& params, RenderContext& ctx)
{
	VkDescriptorSet blurSet = view.frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _blurPipeline.get_descriptor_set_layout(0));

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
