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
	auto downShader = resourceManager->load<Shader>("bloom_downsample.frag", VK_SHADER_STAGE_FRAGMENT_BIT, "main");
	auto upShader = resourceManager->load<Shader>("bloom_upsample.frag", VK_SHADER_STAGE_FRAGMENT_BIT, "main");

	GraphicsPipelineDesc downDesc{
		.shaders = {vert, downShader},
		.colorAttachmentFormats = { VK_FORMAT_R16G16B16A16_SFLOAT },
		.depthAttachmentFormat = VK_FORMAT_UNDEFINED,
		.depthState = {.depthTest = false, .depthWrite = false},
		.rasterState = {.cullMode = VK_CULL_MODE_NONE, .polygonMode = VK_POLYGON_MODE_FILL}
	};
	_downsamplePipeline = GraphicsPipeline(_vulkanCore->get_device(), resourceManager, downDesc);

	GraphicsPipelineDesc upDesc = downDesc;
	upDesc.shaders = { vert, upShader };
	upDesc.blendMode = BlendMode::Additive; // accumulate lower mips into higher mips
	_upsamplePipeline = GraphicsPipeline(_vulkanCore->get_device(), resourceManager, upDesc);
}

void BloomPass::cleanup()
{
	_downsamplePipeline.cleanup();
	_upsamplePipeline.cleanup();
	_vulkanCore = nullptr;
}

void BloomPass::add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view)
{
	if (cvBloom.Get() == 0) // bloom disabled
		return;

	auto& targets = blackboard.get<SceneRenderContext>();
	
	// build mip chain extents
	std::vector<RGTextureHandle> bloomMips;
	std::vector<VkExtent2D> mipExtents;

	VkExtent2D currentExtent = view.drawExtent;
	for (int i = 0; i < _mipCount; i++) {
		currentExtent.width = std::max(1u, currentExtent.width / 2);
		currentExtent.height = std::max(1u, currentExtent.height / 2);

		RGTextureDesc mipDesc{
			.extent = { currentExtent.width, currentExtent.height, 1 },
			.format = VK_FORMAT_R16G16B16A16_SFLOAT,
			.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
		};
		bloomMips.push_back(graph.create_texture(fmt::format("BloomMip_{}", i), mipDesc));
		mipExtents.push_back(currentExtent);

		if (currentExtent.width <= 2 || currentExtent.height <= 2)
			break;
	}

	const int activeMips = static_cast<int>(bloomMips.size());
	if (activeMips == 0)
		return;

	// progressive downsample chain
	for (int i = 0; i < activeMips; i++) {
		RGTextureHandle srcHandle = (i == 0) ? targets.sceneColor : bloomMips[i - 1]; // use scene color for first mip
		RGTextureHandle dstHandle = bloomMips[i];
		VkExtent2D srcExtent = (i == 0) ? view.drawExtent : mipExtents[i - 1]; // use view extent for first mip
		VkExtent2D dstExtent = mipExtents[i];

		// downsample pass
		graph.add_pass<BloomDownsampleParams>(fmt::format("BloomDownsample_{}", i),
			[&, srcHandle, dstHandle, srcExtent, dstExtent, i](RGPassBuilder& builder, BloomDownsampleParams& params) {
				params.src = builder.read_texture(srcHandle, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				params.dst = builder.set_color_attachment(dstHandle, VK_ATTACHMENT_LOAD_OP_CLEAR, { 0.0f, 0.0f, 0.0f, 1.0f });
				params.srcTexelSize = glm::vec2(1.0f / static_cast<float>(srcExtent.width), 1.0f / static_cast<float>(srcExtent.height));
				params.mipLevel = i;
				builder.set_extent(dstExtent);
			},
			[this, &view, &graph](const BloomDownsampleParams& params, RenderContext& rc) {
				render_downsample(view, graph, params, rc);
			});
	}

	// progressive upsample chain
	for (int i = activeMips - 2; i >= 0; i--) {
		RGTextureHandle srcHandle = bloomMips[i + 1];
		RGTextureHandle dstHandle = bloomMips[i];
		VkExtent2D dstExtent = mipExtents[i];

		graph.add_pass<BloomUpsampleParams>(fmt::format("BloomUpsample_{}", i),
			[&, srcHandle, dstHandle, dstExtent](RGPassBuilder& builder, BloomUpsampleParams& params) {
				params.src = builder.read_texture(srcHandle, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				params.dst = builder.set_color_attachment(dstHandle, VK_ATTACHMENT_LOAD_OP_LOAD); // load existing higher resolution mip
				builder.set_extent(dstExtent);
			},
			[this, &view, &graph](const BloomUpsampleParams& params, RenderContext& rc) {
				render_upsample(view, graph, params, rc);
			});
	}

	targets.bloomTexture = bloomMips[0];
}

void BloomPass::on_imgui()
{
	ImGui::SliderInt("Mip Chain Depth", &_mipCount, 2, static_cast<int>(MAX_BLOOM_MIPS));
	ImGui::SliderFloat("Threshold", &_threshold, 0.0f, 5.0f);
	ImGui::SliderFloat("Soft Knee", &_knee, 0.01f, 2.0f);
	ImGui::SliderFloat("Tent Filter Radius", &_filterRadius, 0.0005f, 0.02f, "%.4f");
}

void BloomPass::render_downsample(const RenderViewContext& view, const RenderGraph& graph, const BloomDownsampleParams& params, RenderContext& ctx)
{
	VkDescriptorSet set = view.frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _downsamplePipeline.get_descriptor_set_layout(0));

	std::unordered_map<std::string, VkDescriptorImageInfo> images = {
		{ "srcTexture", { DefaultResources::get().defaultSamplerLinear->get_sampler(), graph.get_image_view(params.src), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}
	};
	DescriptorBinder::update_set(_vulkanCore->get_device(), set, _downsamplePipeline.get_reflection().sets.at(0), {}, images);

	ctx.bind_pipeline(_downsamplePipeline);
	ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _downsamplePipeline.get_layout(), 0, set);

	PushConstantDispatcher pc(_downsamplePipeline.get_reflection());
	pc.set("srcTexelSize", params.srcTexelSize);
	pc.set("threshold", _threshold);
	pc.set("knee", _knee);
	pc.set("mipLevel", params.mipLevel);
	pc.flush(ctx.get_cmd(), _downsamplePipeline.get_layout());

	ctx.set_cull_mode(VK_CULL_MODE_NONE);

	// draw fullscreen triangle
	ctx.draw(3, 1, 0, 0);
}

void BloomPass::render_upsample(const RenderViewContext& view, const RenderGraph& graph, const BloomUpsampleParams& params, RenderContext& ctx)
{
	VkDescriptorSet set = view.frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _upsamplePipeline.get_descriptor_set_layout(0));

	std::unordered_map<std::string, VkDescriptorImageInfo> images = {
		{ "srcTexture", { DefaultResources::get().defaultSamplerLinear->get_sampler(), graph.get_image_view(params.src), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}
	};
	DescriptorBinder::update_set(_vulkanCore->get_device(), set, _upsamplePipeline.get_reflection().sets.at(0), {}, images);

	ctx.bind_pipeline(_upsamplePipeline);
	ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _upsamplePipeline.get_layout(), 0, set);

	PushConstantDispatcher pc(_upsamplePipeline.get_reflection());
	pc.set("filterRadius", _filterRadius);
	pc.flush(ctx.get_cmd(), _upsamplePipeline.get_layout());

	ctx.set_cull_mode(VK_CULL_MODE_NONE);

	// draw fullscreen triangle
	ctx.draw(3, 1, 0, 0);
}
