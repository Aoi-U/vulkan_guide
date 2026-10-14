#pragma once

#include <graphics/pass/RenderFeature.h>
#include <graphics/Pipeline.h>

class BloomPass : public RenderFeature
{
public:
	BloomPass()
		: RenderFeature("BloomPass", true) {}

	void init(VulkanCore* vulkanCore, ResourceManager* resourceManager) override;
	void cleanup() override;

	void add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view) override;

	void on_imgui() override;

private:
	void render_extract(const RenderViewContext& view, const RenderGraph& graph, const BloomBrightExtractPassParameters& params, RenderContext& ctx);
	void render_blur(const RenderViewContext& view, const RenderGraph& graph, const GaussianBlurPassParameters& params, RenderContext& ctx);

	VulkanCore* _vulkanCore{ nullptr };
	GraphicsPipeline _brightExtractPipeline;
	GraphicsPipeline _blurPipeline;
	int _blurIterations{ 10 };
};