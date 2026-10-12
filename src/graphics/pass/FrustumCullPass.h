#pragma once
#include <graphics/pass/ComputePass.h>
#include <graphics/VulkanCore.h>
#include <graphics/RenderGraph.h>

// frustum cull push constants
struct CullPushConstants
{
  glm::vec4 frustumPlanes[6];
  uint32_t drawCount;
  uint32_t cullMode{ 1 }; // enable or disable
};

struct CullStatsData {
  uint32_t visibleDraws{ 0 };
  uint32_t visibleTriangles{ 0 };
};

class FrustumCullPass : public ComputePass
{
public:
  void init(VulkanCore* vulkanCore, ResourceManager* resourceManager) override;
  void cleanup() override;

  void dispatch(RenderContext& ctx, FrameData& frameData, DrawContext& drawCtx, const GPUSceneData& sceneData, VkExtent2D drawExtent, const FrustumCullPassParameters& params, const RenderGraph& graph);

private:

  inline void get_frustum_planes(const glm::mat4& viewProj, glm::vec4 planes[6]) {

    glm::mat4 m = glm::transpose(viewProj);
    planes[0] = m[3] + m[0]; // left
    planes[1] = m[3] - m[0]; // right
    planes[2] = m[3] + m[1]; // bottom
    planes[3] = m[3] - m[1]; // top
    planes[4] = m[2];        // near plane for [0, 1] range
    planes[5] = m[3] - m[2]; // far plane for [0, 1] range

    // normalize planes
    for (int i = 0; i < 6; ++i) {
      float len = glm::length(glm::vec3(planes[i]));
      if (len > 0.0f) {
        planes[i] /= len;
      }
    }
  }

  VulkanCore* _vulkanCore{ nullptr };
  ComputePipeline _pipeline;
};
