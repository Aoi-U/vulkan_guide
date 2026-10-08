#include "SSAOBlurPass.h"

#include <utils/vk_descriptors.h>
#include <graphics/Renderer.h>
#include <chrono>
#include <resources/DefaultResources.h>

void SSAOBlurPass::init(VulkanCore* vulkanCore, ResourceManager* resourceManager, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat)
{
  _vulkanCore = vulkanCore;

  DescriptorLayoutBuilder builder;
  builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 0: blur input texture
  _descriptorLayout = builder.build(_vulkanCore->get_device(), VK_SHADER_STAGE_FRAGMENT_BIT);

  auto vert = resourceManager->load<Shader>("deferred_vert", VK_SHADER_STAGE_VERTEX_BIT, "main", "../../shaders/deferred_lighting.vert.spv");
  auto frag = resourceManager->load<Shader>("ssao_blur_frag", VK_SHADER_STAGE_FRAGMENT_BIT, "main", "../../shaders/ssao_blur.frag.spv");

  GraphicsPipelineDesc desc{
    .shaders = { vert, frag },
    .descriptorLayouts = { _descriptorLayout },
    .colorAttachmentFormats = { colorFormat },
    .depthAttachmentFormat = VK_FORMAT_UNDEFINED,
    .depthState = {.depthTest = false, .depthWrite = false },
    .rasterState = {.cullMode = VK_CULL_MODE_NONE, .polygonMode = VK_POLYGON_MODE_FILL }
  };
  _pipeline = GraphicsPipeline(_vulkanCore->get_device(), desc);
}

void SSAOBlurPass::cleanup()
{
  _pipeline.cleanup();
  if (_descriptorLayout != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(_vulkanCore->get_device(), _descriptorLayout, nullptr);
    _descriptorLayout = VK_NULL_HANDLE;
  }
  _vulkanCore = nullptr;
}

void SSAOBlurPass::render(RenderContext & ctx, FrameData & frameData, const std::unordered_map<std::string, ResourceHandle<Texture>> renderTargets, DrawContext & drawCtx, const GPUSceneData & sceneData)
{
  // begin clock
  auto start = std::chrono::system_clock::now();

  VkDescriptorSet blurSet = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _descriptorLayout);
  DescriptorWriter writer;
  writer.write_image(0, renderTargets.find("SSAOColor")->second->get_image_view(), DefaultResources::get().defaultSamplerLinear->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.update_set(_vulkanCore->get_device(), blurSet);

  ctx.bind_pipeline(_pipeline);
  ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.get_layout(), 0, blurSet);

  // draw full screen triangle
  ctx.draw(3, 1, 0, 0);

  auto end = std::chrono::system_clock::now();
  _stats.clear();
  _stats.name = "SSAO Blur Pass";
  _stats.passTimeMs = std::chrono::duration<float, std::milli>(end - start).count();
}
