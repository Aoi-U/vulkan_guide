#pragma once

#include <graphics/pass/Pass.h>
#include <core/vk_types.h>
#include <graphics/RenderContext.h>

class VulkanCore;
class RenderResourceManager;
struct FrameData;
struct DrawContext;

class RenderPass : public Pass {
public:
	virtual void init(VulkanCore* vulkanCore, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat) = 0;
	virtual void render(RenderContext& ctx, FrameData& frameData, RenderResourceManager& resources, DrawContext& drawCtx, const GPUSceneData& sceneData, VkExtent2D drawExtent) = 0;
};

