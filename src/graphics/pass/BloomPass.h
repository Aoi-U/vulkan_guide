#pragma once

#include <graphics/pass/RenderFeature.h>
#include <graphics/Pipeline.h>
#include <graphics/utils/RGParameters.h>

class BloomPass : public RenderFeature
{
public:
	static constexpr uint32_t MAX_BLOOM_MIPS = 6;

	BloomPass()
		: RenderFeature("BloomPass", true) {}

	void init(VulkanCore* vulkanCore, ResourceManager* resourceManager) override;
	void cleanup() override;

	void add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view) override;

	void on_imgui() override;

private:
	void render_downsample(const RenderViewContext& view, const RenderGraph& graph, const BloomDownsampleParams& params, RenderContext& ctx);
	void render_upsample(const RenderViewContext& view, const RenderGraph& graph, const BloomUpsampleParams& params, RenderContext& ctx);

	VulkanCore* _vulkanCore{ nullptr };
	GraphicsPipeline _downsamplePipeline;
	GraphicsPipeline _upsamplePipeline;
	
	float _threshold{ 1.0f };
	float _knee{ 0.5f };
	float _filterRadius{ 0.005f };
	int _mipCount{ 5 };
};