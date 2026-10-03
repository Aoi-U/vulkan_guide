#pragma once 

#include <graphics/pass/Pass.h>
#include <core/vk_types.h>
#include <graphics/RenderContext.h>

class VulkanCore;
struct FrameData;
struct DrawContext;

class ComputePass : public Pass {
public:
  virtual void init(VulkanCore* vulkanCore) = 0;
  virtual void dispatch(RenderContext& ctx, FrameData& frameData, DrawContext& drawCtx, const GPUSceneData& sceneData, VkExtent2D drawExtent) = 0;
};