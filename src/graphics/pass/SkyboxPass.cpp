#include "SkyboxPass.h"
#include <graphics/reflection/DescriptorBinder.h>
#include <graphics/Renderer.h>
#include <chrono>

void SkyboxPass::init(VulkanCore* vulkanCore, ResourceManager* resourceManager)
{
	_vulkanCore = vulkanCore;

	// load shaders
	auto vert = resourceManager->load<Shader>("skybox.vert", VK_SHADER_STAGE_VERTEX_BIT, "main");
	auto frag = resourceManager->load<Shader>("skybox.frag", VK_SHADER_STAGE_FRAGMENT_BIT, "main");

	// create pipeline
	GraphicsPipelineDesc pipelineDesc{
		.shaders = {vert, frag},
		.colorAttachmentFormats = { VK_FORMAT_R16G16B16A16_SFLOAT },
		.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT,
		.depthState = {.depthTest = true, .depthWrite = false, .compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL}, // test GREATER_OR_EQUAL to draw only on empty pixels without writing depth
		.rasterState = {.cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_CLOCKWISE, .polygonMode = VK_POLYGON_MODE_FILL},
		.blendMode = BlendMode::None
	};
	_pipeline = GraphicsPipeline(_vulkanCore->get_device(), resourceManager, pipelineDesc);
}

void SkyboxPass::cleanup()
{
	VkDevice dev = _vulkanCore->get_device();
	_pipeline.cleanup();
	_vulkanCore = nullptr;
}

void SkyboxPass::add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view)
{
	auto& targets = blackboard.get<SceneRenderContext>();

	graph.add_pass<SkyboxGraphParams>("SkyboxPass",
		[&](RGPassBuilder& builder, SkyboxGraphParams& params) {
			bool gbufferActive = (targets.gPosition != targets.defaultBlack);
			VkAttachmentLoadOp depthLoadOp = gbufferActive ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;

			params.envCubemap = builder.read_texture(targets.envCubemap, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			params.sceneColor = builder.set_color_attachment(targets.sceneColor, VK_ATTACHMENT_LOAD_OP_LOAD);
			params.sceneDepth = builder.set_depth_attachment(targets.sceneDepth, depthLoadOp, 0.0);
			builder.set_extent(view.drawExtent);
		},
		[this, &view, &graph](const SkyboxGraphParams& params, RenderContext& ctx) {
			VkDescriptorSet skyboxSet = view.frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _pipeline.get_descriptor_set_layout(0));

			std::unordered_map<std::string, VkDescriptorBufferInfo> buffers = {
				{"sceneData", { view.frameData.sceneDataBuffer->get_buffer(), 0, sizeof(GPUSceneData) }}
			};
			std::unordered_map<std::string, VkDescriptorImageInfo> images = {
				{"skyMap", { DefaultResources::get().cubemapSampler->get_sampler(), graph.get_image_view(params.envCubemap), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}
			};

			DescriptorBinder::update_set(_vulkanCore->get_device(), skyboxSet, _pipeline.get_reflection().sets.at(0), buffers, images);

			ctx.bind_pipeline(_pipeline);
			ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.get_layout(), 0, skyboxSet);

			// draw full scren triangle
			ctx.set_cull_mode(VK_CULL_MODE_NONE);
			ctx.draw(3, 1, 0, 0);
		});
}
