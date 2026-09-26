#pragma once
#include <graphics/pass/RenderPass.h>
#include <graphics/VulkanCore.h>

class FrustumCullPass : public RenderPass
{
public:
  void init(VulkanCore* vulkanCore, VkFormat colorFormat, VkFormat depthFormat) override;
  void cleanup() override;
  void render(VkCommandBuffer cmd, FrameData& frameData, DrawContext& ctx, const GPUSceneData& sceneData, VkExtent2D drawExtent) override;

private:
  void build_cull_pipeline();

  inline void get_frustum_planes(const glm::mat4& viewProj, glm::vec4 planes[6]) {

    glm::mat4 m = glm::transpose(viewProj);
    planes[0] = m[3] + m[0]; // left
    planes[1] = m[3] - m[0]; // right
    planes[2] = m[3] + m[1]; // bottom
    planes[3] = m[3] - m[1]; // top
    planes[4] = m[2];        // near plane for [0, 1] range
    planes[5] = m[3] - m[2]; // far plane for [0, 1] range

    for (int i = 0; i < 6; ++i) {
      float len = glm::length(glm::vec3(planes[i]));
      if (len > 0.0f) {
        planes[i] /= len;
      }
    }
  }

  VulkanCore* _vulkanCore{ nullptr };
  VkDescriptorSetLayout _cullDescriptorLayout{ VK_NULL_HANDLE };
  VkPipelineLayout _cullPipelineLayout{ VK_NULL_HANDLE };
  VkPipeline _cullPipeline{ VK_NULL_HANDLE };
};
