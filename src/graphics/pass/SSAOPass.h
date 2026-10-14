#pragma once

#include <graphics/pass/RenderFeature.h>
#include <graphics/Pipeline.h>
#include <graphics/VulkanCore.h>
#include <graphics/RenderGraph.h>
#include <resources/Buffer.h>
#include <resources/Sampler.h>

class SSAOPass : public RenderFeature
{
public:
	SSAOPass()
		: RenderFeature("SSAOPass", true) {}

	void init(VulkanCore* vulkanCore, ResourceManager* resourceManager) override;
	void cleanup() override;

	void add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view) override;

	void on_imgui() override;

private:
	void generate_kernal_and_noise();

	void ssaoSubpass(const RenderViewContext& view, const RenderGraph& graph, const SSAOPassParameters& params, RenderContext& rc);
	void blurSubpass(const RenderViewContext& view, const RenderGraph& graph, const SSAOBlurPassParameters& params, RenderContext& rc);

	VulkanCore* _vulkanCore{ nullptr };
	ResourceManager* _resourceManager{ nullptr };

	GraphicsPipeline _ssaoPipeline;
	GraphicsPipeline _blurPipeline;

	ResourceHandle<Buffer> _kernelBuffer;
	ResourceHandle<Texture> _noiseImage;
	ResourceHandle<Sampler> _noiseSampler;
};