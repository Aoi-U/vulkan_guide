#include "DeferredLightingPass.h"

#include <utils/vk_descriptors.h>
#include <graphics/Renderer.h>
#include <chrono>
#include <resources/DefaultResources.h>

void DeferredLightingPass::init(VulkanCore* vulkanCore, ResourceManager* resourceManager, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat)
{
  _vulkanCore = vulkanCore;

  DescriptorLayoutBuilder builder;
  builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);         // binding 0: uniform scene data
  builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 1: gPosition
  builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 2: gNormal
  builder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 3: gAlbedo
  builder.add_binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 4: gMetalRough
  builder.add_binding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 5: shadowmap
  builder.add_binding(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 6: irradiance
  builder.add_binding(7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 7: prefilter
  builder.add_binding(8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 8: brdf lut
  builder.add_binding(9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 9: point shadow map
  builder.add_binding(10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 10: ssao map
  builder.add_binding(11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 11:: gEmissive
  _descriptorLayout = builder.build(_vulkanCore->get_device(), VK_SHADER_STAGE_FRAGMENT_BIT);

  auto vert = resourceManager->load<Shader>("deferred_lighting.vert", VK_SHADER_STAGE_VERTEX_BIT, "main");
  auto frag = resourceManager->load<Shader>("deferred_lighting.frag", VK_SHADER_STAGE_FRAGMENT_BIT, "main");

  GraphicsPipelineDesc desc{
    .shaders = { vert, frag },
    .descriptorLayouts = { _descriptorLayout },
    .pushConstants = {{ .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(int) }}, // push constant for enabling ssao
    .colorAttachmentFormats = { colorFormat },
    .depthAttachmentFormat = VK_FORMAT_UNDEFINED, // no depth attachment for fullscreen pass
    .depthState = {.depthTest = false, .depthWrite = false },
    .rasterState = {.cullMode = VK_CULL_MODE_NONE, .polygonMode = VK_POLYGON_MODE_FILL }
  };
  _pipeline = GraphicsPipeline(_vulkanCore->get_device(), desc);
}

void DeferredLightingPass::cleanup()
{
  _pipeline.cleanup();
  if (_descriptorLayout != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(_vulkanCore->get_device(), _descriptorLayout, nullptr);
    _descriptorLayout = VK_NULL_HANDLE;
  }
  _vulkanCore = nullptr;
}

void DeferredLightingPass::render(RenderContext& ctx, FrameData& frameData, DrawContext& drawCtx, const GPUSceneData& sceneData, const DeferredPassParameters& params, const RenderGraph& graph)
{
  // begin clock
  auto start = std::chrono::system_clock::now();

  const auto& env = drawCtx.environment;

  // update descriptor set
  VkDescriptorSet lightingSet = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _descriptorLayout);
  DescriptorWriter writer;
  writer.write_buffer(0, frameData.sceneDataBuffer->get_buffer(), sizeof(GPUSceneData), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
  writer.write_image(1, graph.get_image_view(params.gPosition), DefaultResources::get().defaultSamplerNearest->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
  writer.write_image(2, graph.get_image_view(params.gNormal), DefaultResources::get().defaultSamplerNearest->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
  writer.write_image(3, graph.get_image_view(params.gAlbedo), DefaultResources::get().defaultSamplerNearest->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
  writer.write_image(4, graph.get_image_view(params.gMetalRough), DefaultResources::get().defaultSamplerNearest->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
  writer.write_image(5, graph.get_image_view(params.shadowMap), DefaultResources::get().shadowSampler->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
  writer.write_image(6, env.irradianceView, env.cubemapSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
  writer.write_image(7, env.prefilterView, env.cubemapSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
  writer.write_image(8, env.brdfLutView, env.lutSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
  writer.write_image(9, graph.get_image_view(params.pointShadowMap), env.cubemapSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
  writer.write_image(10, graph.get_image_view(params.ssao), DefaultResources::get().defaultSamplerLinear->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
  writer.write_image(11, graph.get_image_view(params.gEmissive), DefaultResources::get().defaultSamplerNearest->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
  writer.update_set(_vulkanCore->get_device(), lightingSet);

  ctx.bind_pipeline(_pipeline);
  ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.get_layout(), 0, lightingSet);

  int enableSSAO = cvSSAO.Get();
  ctx.set_push_constants(_pipeline.get_layout(), VK_SHADER_STAGE_FRAGMENT_BIT, enableSSAO);
  
  ctx.draw(3, 1, 0, 0); // draw fullscreen triangle

  auto end = std::chrono::system_clock::now();
  _stats.clear();
  _stats.name = "Deferred Lighting Pass";
  _stats.passTimeMs = std::chrono::duration<float, std::milli>(end - start).count();
}
