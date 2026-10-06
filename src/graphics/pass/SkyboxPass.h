#pragma once

#include <graphics/pass/RenderPass.h>
#include <graphics/VulkanCore.h>
#include <graphics/RenderResourceManager.h>

class SkyboxPass : public RenderPass
{
public:
  void init(VulkanCore* vulkanCore, ResourceManager* resourceManager, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat) override;
  void cleanup() override;
  void render(RenderContext& ctx, FrameData& frameData, RenderResourceManager& resources, DrawContext& drawCtx, const GPUSceneData& sceneData) override;

private:
  VulkanCore* _vulkanCore{ nullptr };
  VkDescriptorSetLayout _descriptorLayout{ VK_NULL_HANDLE };
  GraphicsPipeline _pipeline;
};