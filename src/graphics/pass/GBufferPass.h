#pragma once

#include <graphics/pass/RenderPass.h>
#include <graphics/Pipeline.h>

class GBufferPass : public RenderPass {
public:
  void init(VulkanCore* vulkanCore, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat) override;
  void cleanup() override;
  void render(RenderContext& ctx, FrameData& frameData, RenderResourceManager& resources, DrawContext& drawCtx, const GPUSceneData& sceneData) override;

private:
  VulkanCore* _vulkanCore{ nullptr };
  VkDescriptorSetLayout _sceneDataDescriptorLayout{ VK_NULL_HANDLE };
  GraphicsPipeline _pipeline;
};