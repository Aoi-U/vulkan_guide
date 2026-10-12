#pragma once

#include <graphics/pass/RenderPass.h>
#include <graphics/VulkanCore.h>
#include <graphics/RenderGraph.h>

class SkyboxPass : public RenderPass
{
public:
  void init(VulkanCore* vulkanCore, ResourceManager* resourceManager, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat) override;
  void cleanup() override;

  void render(RenderContext& ctx, FrameData& frameData, DrawContext& drawCtx, const GPUSceneData& sceneData, const SkyboxPassParameters& params, const RenderGraph& graph);

private:
  VulkanCore* _vulkanCore{ nullptr };
  GraphicsPipeline _pipeline;
};