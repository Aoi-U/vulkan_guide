#pragma once

#include <graphics/RenderGraph.h>
#include <graphics/RGBlackboard.h>
#include <string>

class VulkanCore;
class ResourceManager;
struct FrameData;
struct DrawContext;
struct GPUSceneData;

struct RenderViewContext
{
	FrameData& frameData;
	DrawContext& drawCtx;
	const GPUSceneData& sceneData;
	VkExtent2D drawExtent;
	uint32_t frameNumber{ 0 };
};

class RenderFeature
{
public:
	explicit RenderFeature(std::string name, bool enabledByDefault = true)
		: _name(std::move(name)), _enabled(enabledByDefault) {}

	virtual ~RenderFeature() = default;

	virtual void init(VulkanCore* core, ResourceManager* rm) = 0;
	virtual void cleanup() = 0;

	virtual void add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view) = 0;

	virtual void on_imgui() {};

	const std::string& get_name() const { return _name; }
	bool is_enabled() const { return _enabled; }
	void set_enabled(bool enabled) { _enabled = enabled; }

private:
	std::string _name;
	bool _enabled{ true };
};