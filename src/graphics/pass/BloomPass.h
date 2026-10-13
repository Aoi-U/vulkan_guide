#pragma once

#include <graphics/pass/RenderPass.h>
#include <graphics/Pipeline.h>
#include <graphics/RenderGraph.h>
#include <graphics/utils/RGParameters.h>

class BloomPass : public RenderPass
{
public:
	void init(VulkanCore* vulkanCore, ResourceManager* resourceManager, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat) override;
	void cleanup() override;

	void render_extract(RenderContext& ctx, FrameData& frameData, const BloomBrightExtractPassParameters& params, const RenderGraph& graph);
	void render_blur(RenderContext& ctx, FrameData& frameData, const GaussianBlurPassParameters& params, const RenderGraph& graph);

private:
	VulkanCore* _vulkanCore{ nullptr };
	GraphicsPipeline _brightExtractPipeline;
	GraphicsPipeline _blurPipeline;
};