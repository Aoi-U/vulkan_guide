#pragma once

#include <graphics/pass/RenderPass.h>
#include <graphics/MaterialSystem.h>
#include <graphics/VulkanCore.h>
#include <graphics/RenderResourceManager.h>

class ForwardPass : public RenderPass
{
public:
  void init(VulkanCore* vulkanCore, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat) override;
  void cleanup() override;
  void render(RenderContext& ctx, FrameData& frameData, RenderResourceManager& resources, DrawContext& drawCtx, const GPUSceneData& sceneData) override;

private:

  VulkanCore* _vulkanCore;
  VkDescriptorSetLayout _sceneDataDescriptorLayout;

  GraphicsPipeline _opaquePipeline;
  GraphicsPipeline _transparentPipeline;
};

