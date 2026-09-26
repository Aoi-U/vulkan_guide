#pragma once

#include <core/vk_types.h>
#include <vulkan/vulkan.h>
#include <string>

class VulkanCore;
struct FrameData;

struct PassStats {
	std::string name;
	uint32_t drawcallCount;
	uint32_t triangleCount;
	float passTime;
};

class RenderPass {
public:

	virtual ~RenderPass() = default;

	virtual void init(VulkanCore* vulkanCore, VkFormat colorFormat, VkFormat depthFormat) = 0;
	virtual void cleanup() = 0;
	virtual void render(VkCommandBuffer cmd, FrameData& frameData, DrawContext& context, const GPUSceneData& sceneData, VkExtent2D drawExtent) = 0;

	const PassStats get_stats() { return _stats; }

protected:
	PassStats _stats{};
};

