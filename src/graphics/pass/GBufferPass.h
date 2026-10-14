#pragma once

#include <graphics/pass/RenderFeature.h>
#include <graphics/Pipeline.h>
#include <graphics/RenderGraph.h>
#include <graphics/utils/vk_types.h>

class GBufferPass : public RenderFeature {
public:
  GBufferPass()
    : RenderFeature("GBufferPass", true) {}

  void init(VulkanCore* vulkanCore, ResourceManager* resourceManager) override;
  void cleanup() override;

  void add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view) override;

private:
  VulkanCore* _vulkanCore{ nullptr };
  ResourceManager* _resourceManager{ nullptr };
  VkFormat _depthFormat{ VK_FORMAT_D32_SFLOAT };
};