#include "PostProcessPass.h"

#include <graphics/reflection/DescriptorBinder.h>
#include <graphics/reflection/PushConstantDispatcher.h>
#include <graphics/Renderer.h>
#include <resources/DefaultResources.h>
#include <core/cvars.h>
#include <imgui.h>

AutoCVar_Float cvExposure("r.exposure", "Camera exposure multiplier", 1.0f, CVarFlags::EditFloatSlider, 0.1f, 10.0f);

void PostProcessPass::init(VulkanCore* vulkanCore, ResourceManager* resourceManager)
{
	_vulkanCore = vulkanCore;
	auto vert = resourceManager->load<Shader>("deferred_lighting.vert", VK_SHADER_STAGE_VERTEX_BIT, "main");
	auto frag = resourceManager->load<Shader>("post_process.frag", VK_SHADER_STAGE_FRAGMENT_BIT, "main");

	GraphicsPipelineDesc desc{
		.shaders = { vert, frag},
		.colorAttachmentFormats = { VK_FORMAT_R8G8B8A8_UNORM },
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

void PostProcessPass::add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view)
{
	auto& targets = blackboard.get<SceneRenderContext>();

	RGTextureDesc finalColorDesc{
		.extent = { view.drawExtent.width, view.drawExtent.height, 1 },
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
	};
	targets.finalColor = graph.create_texture("FinalColor", finalColorDesc);

	graph.add_pass<PostProcessPassParameters>("PostProcessPass",
		[&](RGPassBuilder& builder, PostProcessPassParameters& params) {
			params.bloomColor = builder.read_texture(targets.bloomTexture, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			params.sceneColor = builder.read_texture(targets.sceneColor, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			params.finalColor = builder.set_color_attachment(targets.finalColor, VK_ATTACHMENT_LOAD_OP_CLEAR, { 0.0f, 0.0f, 0.0f, 1.0f });
			builder.set_extent(view.drawExtent);
		},
		[this, &view, &graph](const PostProcessPassParameters& params, RenderContext& rc) {
			VkDescriptorSet postProcessSet = view.frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _pipeline.get_descriptor_set_layout(0));
			VkSampler linear = DefaultResources::get().defaultSamplerLinear->get_sampler();

			std::unordered_map<std::string, VkDescriptorImageInfo> images = {
				{"sceneColor", { linear, graph.get_image_view(params.sceneColor), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }},
				{"bloomColor", { linear, graph.get_image_view(params.bloomColor), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}
			};

			DescriptorBinder::update_set(_vulkanCore->get_device(), postProcessSet, _pipeline.get_reflection().sets.at(0), {}, images);

			rc.bind_pipeline(_pipeline);
			rc.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.get_layout(), 0, postProcessSet);

			// set push constants
			PushConstantDispatcher pc(_pipeline.get_reflection());
			pc.set("exposure", static_cast<float>(cvExposure.Get()));
			pc.flush(rc.get_cmd(), _pipeline.get_layout());

			rc.set_cull_mode(VK_CULL_MODE_NONE);

			// draw fullscreen triangle
			rc.draw(3, 1, 0, 0);
		}
	);
}

void PostProcessPass::on_imgui()
{
	float exposure = static_cast<float>(cvExposure.Get());
	if (ImGui::SliderFloat("Exposure", &exposure, 0.1f, 10.0f)) {
		cvExposure.Set(exposure);
	}
}

