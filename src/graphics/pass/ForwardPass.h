#pragma once

#include <graphics/pass/RenderPass.h>
#include <graphics/Materials.h>
#include <graphics/VulkanCore.h>
#include <vulkan/vulkan_core.h>


class ForwardPass : public RenderPass
{
public:
  void init(VulkanCore* vulkanCore, VkFormat colorFormat, VkFormat depthFormat) override;
  void cleanup() override;
  void render(VkCommandBuffer cmd, FrameData& frameData, const DrawContext& ctx, const GPUSceneData& sceneData, VkExtent2D drawExtent) override;

	GLTFMetallic_Roughness* get_material_system() { return &_metalRoughMaterial; }

private:
  bool is_visible(const RenderObject& obj, const glm::mat4& viewProj);

  VulkanCore* _vulkanCore;
  VkDescriptorSetLayout _sceneDataDescriptorLayout;
  GLTFMetallic_Roughness _metalRoughMaterial;
};

