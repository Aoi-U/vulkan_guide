#pragma once

#include <graphics/pass/RenderFeature.h>
#include <graphics/Pipeline.h>
#include <graphics/RenderGraph.h>

class ShadowPass : public RenderFeature
{
public:
	ShadowPass()
		: RenderFeature("DirectionalShadowPass", true) {}

	void init(VulkanCore* vulkanCore, ResourceManager* resourceManager) override;
	void cleanup() override;

	void add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view) override;

private:
	VulkanCore* _vulkanCore{ nullptr };
	ResourceManager* _resourceManager{ nullptr };
	GraphicsPipeline _pipeline;
};