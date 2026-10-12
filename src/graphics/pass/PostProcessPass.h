#pragma once

#include <graphics/pass/RenderPass.h>
#include <graphics/Pipeline.h>
#include <graphics/RenderGraph.h>
#include <RenderContext.h>

class PostProcessPass : public RenderPass
{
public:
	void init(VulkanCore* vulkanCore, ResourceManager* resourceManager, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat) override;
	void cleanup() override;

	void render(RenderContext& ctx, FrameData& frameData, const PostProcessPassParameters& params, const RenderGraph& graph);

private:
	VulkanCore* _vulkanCore{ nullptr };
	GraphicsPipeline _pipeline;
};