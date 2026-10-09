#pragma once

#include <graphics/pass/RenderPass.h>
#include <graphics/Pipeline.h>
#include <graphics/RenderGraph.h>

class DeferredLightingPass : public RenderPass {
public:
  void init(VulkanCore* vulkanCore, ResourceManager* resourceManager, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat) override;
  void cleanup() override;

  void render(RenderContext& ctx, FrameData& frameData, DrawContext& drawCtx, const GPUSceneData& sceneData, const DeferredPassParameters& params, const RenderGraph& graph);

private:
  VulkanCore* _vulkanCore{ nullptr };
  VkDescriptorSetLayout _descriptorLayout{ VK_NULL_HANDLE };
  GraphicsPipeline _pipeline;
};