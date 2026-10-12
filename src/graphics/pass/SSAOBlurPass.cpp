#include "SSAOBlurPass.h"

#include <graphics/reflection/DescriptorBinder.h>
#include <graphics/Renderer.h>
#include <chrono>
#include <resources/DefaultResources.h>

void SSAOBlurPass::init(VulkanCore* vulkanCore, ResourceManager* resourceManager, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat)
{
  _vulkanCore = vulkanCore;

  auto vert = resourceManager->load<Shader>("deferred_lighting.vert", VK_SHADER_STAGE_VERTEX_BIT, "main");
  auto frag = resourceManager->load<Shader>("ssao_blur.frag", VK_SHADER_STAGE_FRAGMENT_BIT, "main");

  GraphicsPipelineDesc desc{
    .shaders = { vert, frag },
    .colorAttachmentFormats = { colorFormat },
    .depthAttachmentFormat = VK_FORMAT_UNDEFINED,
    .depthState = {.depthTest = false, .depthWrite = false },
    .rasterState = {.cullMode = VK_CULL_MODE_NONE, .polygonMode = VK_POLYGON_MODE_FILL }
  };
  _pipeline = GraphicsPipeline(_vulkanCore->get_device(), resourceManager, desc);
}

void SSAOBlurPass::cleanup()
{
  _pipeline.cleanup();
  _vulkanCore = nullptr;
}

void SSAOBlurPass::render(RenderContext& ctx, FrameData& frameData, DrawContext& drawCtx, const GPUSceneData& data, const SSAOBlurPassParameters& params, const RenderGraph& graph)
{
	VkDescriptorSet blurSet = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _pipeline.get_descriptor_set_layout(0));

  std::unordered_map<std::string, VkDescriptorImageInfo> images = {
    {"ssaoInput", { DefaultResources::get().defaultSamplerLinear->get_sampler(), graph.get_image_view(params.ssao), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }}
  };

	DescriptorBinder::update_set(_vulkanCore->get_device(), blurSet, _pipeline.get_reflection().sets.at(0), {}, images);

  ctx.bind_pipeline(_pipeline);
  ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.get_layout(), 0, blurSet);

  ctx.set_cull_mode(VK_CULL_MODE_NONE);

  // draw full screen triangle
  ctx.draw(3, 1, 0, 0);
}
