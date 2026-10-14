#pragma once

#include <graphics/pass/RenderFeature.h>
#include <graphics/Pipeline.h>
#include <graphics/RenderGraph.h>
#include <RenderContext.h>

class PostProcessPass : public RenderFeature
{
public:
	PostProcessPass()
		: RenderFeature("PostProcessPass", true) {}

	void init(VulkanCore* vulkanCore, ResourceManager* resourceManager) override;
	void cleanup() override;

	void add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view) override;

	void on_imgui() override;

private:
	VulkanCore* _vulkanCore{ nullptr };
	GraphicsPipeline _pipeline;
};