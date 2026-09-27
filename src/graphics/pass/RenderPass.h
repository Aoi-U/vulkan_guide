#pragma once

#include <core/vk_types.h>
#include <vulkan/vulkan.h>
#include <string>
#include <variant>

class VulkanCore;
struct FrameData;


using Metric = std::variant<uint32_t, int32_t, float, std::string>;

struct PassMetric
{
	std::string name;
	Metric value;
	std::string unit = "";
};

struct PassStats {
	std::string name;
	float passTimeMs{ 0.0f };
	std::vector<PassMetric> metrics;

	void clear() {
		metrics.clear();
		passTimeMs = 0.0f;
	}

	void add_metric(std::string label, uint32_t val, std::string unit = "") { metrics.push_back({ std::move(label), val, std::move(unit) }); }
	void add_metric(std::string label, int32_t val, std::string unit = "") { metrics.push_back({ std::move(label), val, std::move(unit) }); }
	void add_metric(std::string label, float val, std::string unit = "") { metrics.push_back({ std::move(label), val, std::move(unit) }); }
	void add_metric(std::string label, std::string val) { metrics.push_back({ std::move(label), std::move(val), "" }); };
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

