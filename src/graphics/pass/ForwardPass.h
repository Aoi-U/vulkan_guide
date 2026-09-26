#pragma once

#include <graphics/pass/RenderPass.h>
#include <graphics/Materials.h>
#include <graphics/VulkanCore.h>

class ForwardPass : public RenderPass
{
public:
  void init(VulkanCore* vulkanCore, VkFormat colorFormat, VkFormat depthFormat) override;
  void cleanup() override;
  void render(VkCommandBuffer cmd, FrameData& frameData, DrawContext& ctx, const GPUSceneData& sceneData, VkExtent2D drawExtent) override;

	GLTFMetallic_Roughness* get_material_system() { return &_metalRoughMaterial; }

private:
  VulkanCore* _vulkanCore;
  VkDescriptorSetLayout _sceneDataDescriptorLayout;

  GLTFMetallic_Roughness _metalRoughMaterial;
};

