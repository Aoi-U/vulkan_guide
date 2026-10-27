#pragma once

#include <graphics/pass/RenderFeature.h>
#include <graphics/RenderGraph.h>

struct VTFeedbackClearParams
{
	RGBufferHandle vtFeedbackBuffer;
};

struct VTFeedbackReadbackParams
{
	RGBufferHandle vtFeedbackBuffer;
	RGBufferHandle vtReadbackBuffer;
};

// pregeometry pass
// zero out gpu feedback atomic requestCount before GBufferPass 
class VTFeedbackClearPass : public RenderFeature
{
public:
	VTFeedbackClearPass()
		: RenderFeature("VTFeedbackClearPass", true) {}

	void init(VulkanCore* core, ResourceManager* rm) override;
	void cleanup() override;

	void add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view) override;

private:
	VulkanCore* _core{ nullptr };
	ResourceManager* _rm{ nullptr };
};

// post geometry pass
// copy populated gpu feedback ssbo into current frames readback buffer
class VTFeedbackPass : public RenderFeature
{
public:
	VTFeedbackPass()
		: RenderFeature("VTFeedbackPass", true) {}

	void init(VulkanCore* core, ResourceManager* rm) override;
	void cleanup() override;

	void add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view) override;

	void on_imgui() override;

private:
	VulkanCore* _core{ nullptr };
	ResourceManager* _rm{ nullptr };
};
