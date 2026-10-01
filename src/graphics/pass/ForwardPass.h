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
  void render(VkCommandBuffer cmd, FrameData& frameData, RenderResourceManager& resources, DrawContext& ctx, const GPUSceneData& sceneData, VkExtent2D drawExtent) override;


private:
  void build_pipelines(VkFormat colorFormat, VkFormat depthFormat);

  VulkanCore* _vulkanCore;
  VkDescriptorSetLayout _sceneDataDescriptorLayout;
  VkDescriptorSetLayout _materialDescriptorLayout;

  MaterialPipeline _opaquePipeline;
  MaterialPipeline _transparentPipeline;
};

