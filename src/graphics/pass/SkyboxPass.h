#pragma once

#include <graphics/pass/RenderPass.h>
#include <graphics/MaterialSystem.h>
#include <graphics/VulkanCore.h>
#include <graphics/RenderResourceManager.h>

class SkyboxPass : public RenderPass
{
public:
  void init(VulkanCore* vulkanCore, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat) override;
  void cleanup() override;
  void render(VkCommandBuffer cmd, FrameData& frameData, RenderResourceManager& resources, DrawContext& ctx, const GPUSceneData& sceneData, VkExtent2D drawExtent) override;

private:
  VulkanCore* _vulkanCore{ nullptr };
  VkDescriptorSetLayout _descriptorLayout{ VK_NULL_HANDLE };
  MaterialPipeline _pipeline;
};