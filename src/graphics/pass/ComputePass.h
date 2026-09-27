#pragma once 

#include <graphics/pass/Pass.h>
#include <core/vk_types.h>

class VulkanCore;
struct FrameData;
struct DrawContext;

class ComputePass : public Pass {
public:
  virtual void init(VulkanCore* vulkanCore) = 0;
  virtual void dispatch(VkCommandBuffer cmd, FrameData& frameData, DrawContext& context, const GPUSceneData& sceneData, VkExtent2D drawExtent) = 0;
};