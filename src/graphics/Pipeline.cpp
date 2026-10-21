#include "graphics/Pipeline.h"
#include <utils/vk_initializers.h>
#include <utils/vk_pipelines.h>
#include <resources/ResourceManager.h>
#include <graphics/reflection/ShaderReflection.h>
#include <fmt/core.h>

void Pipeline::cleanup()
{
  if (_pipeline != VK_NULL_HANDLE && _device != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(_device, _pipeline, nullptr);
    _pipeline = VK_NULL_HANDLE;
  }

  _layout = VK_NULL_HANDLE;
}

Pipeline::Pipeline(Pipeline&& other) noexcept
  : _device(other._device), _pipeline(other._pipeline), _layout(other._layout)
{
  other._device = VK_NULL_HANDLE;
  other._pipeline = VK_NULL_HANDLE;
  other._layout = VK_NULL_HANDLE;
}

Pipeline& Pipeline::operator=(Pipeline&& other) noexcept
{
  if (this != &other)
  {
    cleanup();
    _device = other._device;
    _pipeline = other._pipeline;
    _layout = other._layout;
    other._device = VK_NULL_HANDLE;
    other._pipeline = VK_NULL_HANDLE;
    other._layout = VK_NULL_HANDLE;
  }
  return *this;
}

GraphicsPipeline::GraphicsPipeline(VkDevice device, ResourceManager* resourceManager, const GraphicsPipelineDesc& desc)
  : Pipeline(device)
{
  // reflect shaders
  std::vector<ReflectedShaderStage> reflections;
  for (const auto& shader : desc.shaders)
  {
    if (shader.is_valid())
    {
      ReflectedShaderStage stageRef;
      if (ShaderReflection::reflect_stage(shader->get_spirv_code(), shader->get_stage(), stageRef))
      {
        reflections.push_back(std::move(stageRef));
      }
    }
  }

  // merge reflection stages
  _reflection = ShaderReflection::merge_stages(reflections);

  // get descriptor set layouts from reflection
  std::vector<VkDescriptorSetLayout> layouts;
  uint32_t maxSet = 0;
  if (!_reflection.sets.empty())
  {
    for (const auto& [setIndex, bindings] : _reflection.sets)
    {
      maxSet = std::max(maxSet, setIndex);
    }
    layouts.resize(maxSet + 1, VK_NULL_HANDLE);

    auto& cache = resourceManager->get_pipeline_layout_cache();
    for (const auto& [setIndex, bindings] : _reflection.sets)
    {
      if (setIndex == 1)
      {
        layouts[setIndex] = resourceManager->get_bindless_layout();
        _setLayouts[setIndex] = layouts[setIndex];
      }
      else
      {
        VkDescriptorSetLayout layout = cache.get_descriptor_set_layout(bindings, false);
        layouts[setIndex] = layout;
        _setLayouts[setIndex] = layout;
      }
    }

    for (uint32_t i = 0; i <= maxSet; i++)
    {
      if (layouts[i] == VK_NULL_HANDLE)
      {
        layouts[i] = cache.get_descriptor_set_layout({}, false);
      }
    }
  }

  auto& cache = resourceManager->get_pipeline_layout_cache();
  _layout = cache.get_pipeline_layout(layouts, _reflection.pushConstantRanges);

  // build pipeline
  PipelineBuilder builder;
  builder._pipelineLayout = _layout;

  for (const auto& shader : desc.shaders)
  {
    if (shader.is_valid())
    {
      builder._shaderStages.push_back(shader->get_stage_info());
    }
  }

  builder.set_input_topology(desc.topology);
  builder.set_polygon_mode(desc.rasterState.polygonMode);
  builder.set_cull_mode(desc.rasterState.cullMode);
  builder.set_front_face(desc.rasterState.frontFace);
  builder.set_multisampling_none();
  builder._rasterizer.depthClampEnable = desc.rasterState.depthClamp ? VK_TRUE : VK_FALSE;

  switch (desc.blendMode)
  {
  case BlendMode::None:
    builder.disable_blending();
    break;
  case BlendMode::Additive:
    builder.enable_blending_additive();
    break;
  case BlendMode::AlphaBlend:
    builder.enable_blending_alphablend();
    break;
  }

  if (desc.depthState.depthTest)
  {
    builder.enable_depthtest(desc.depthState.depthWrite, desc.depthState.compareOp);
  }
  else
  {
    builder.disable_depthtest();
  }

  builder.set_depth_format(desc.depthAttachmentFormat);
  builder._renderInfo.colorAttachmentCount = static_cast<uint32_t>(desc.colorAttachmentFormats.size());
  builder._renderInfo.pColorAttachmentFormats = desc.colorAttachmentFormats.data();
  builder.set_dynamic_states(desc.dynamicStates);

  _pipeline = builder.build_pipeline(_device);
}


ComputePipeline::ComputePipeline(VkDevice device, ResourceManager* resourceManager, const ComputePipelineDesc& desc)
  : Pipeline(device)
{
  if (!desc.computeShader.is_valid())
  {
    fmt::println("ComputePipeline: invalid compute shader provided");
    return;
  }

  // reflect shader
  ReflectedShaderStage reflection;
  if (!ShaderReflection::reflect_stage(desc.computeShader->get_spirv_code(), VK_SHADER_STAGE_COMPUTE_BIT, reflection))
  {
    fmt::println("ComputePipeline: failed to reflect compute shader");
    return;
  }
  // merge reflection stages
  _reflection = ShaderReflection::merge_stages({&reflection, 1});

  // get descriptor set layouts from reflection
  std::vector<VkDescriptorSetLayout> layouts;
  uint32_t maxSet = 0;

  auto& cache = resourceManager->get_pipeline_layout_cache();
  if (!_reflection.sets.empty())
  {
    for (const auto& [setIndex, binding] : _reflection.sets)
    {
      maxSet = std::max(maxSet, setIndex);
    }
    layouts.resize(maxSet + 1, VK_NULL_HANDLE);

    for (const auto& [setIndex, bindings] : _reflection.sets)
    {
      if (setIndex == 1)
      {
        layouts[setIndex] = resourceManager->get_bindless_layout();
        _setLayouts[setIndex] = layouts[setIndex];
      }
      else
      {
        VkDescriptorSetLayout layout = cache.get_descriptor_set_layout(bindings, false);
        layouts[setIndex] = layout;
        _setLayouts[setIndex] = layout;
      }
    }

    for (uint32_t i = 0; i <= maxSet; i++)
    {
      if (layouts[i] == VK_NULL_HANDLE)
      {
        layouts[i] = cache.get_descriptor_set_layout({}, false);
      }
    }
  }

  _layout = cache.get_pipeline_layout(layouts, _reflection.pushConstantRanges);

  // build pipeline
  PipelineBuilder builder;
  builder._pipelineLayout = _layout;
  builder._computeShaderStage = desc.computeShader->get_stage_info();
  _pipeline = builder.build_compute_pipeline(_device);
}
