#pragma once

#include <graphics/pass/Pass.h>
#include <graphics/utils/vk_types.h>
#include <graphics/RenderContext.h>
#include <resources/ResourceManager.h>
#include <resources/Texture.h>
#include <unordered_map>

class VulkanCore;
struct FrameData;
struct DrawContext;

class RenderPass : public Pass {
public:
	virtual void init(VulkanCore* vulkanCore, ResourceManager* resourceManager, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat) = 0;
};

