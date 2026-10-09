#pragma once

#include <graphics/pass/RenderPass.h>
#include <graphics/Pipeline.h>
#include <graphics/VulkanCore.h>
#include <graphics/RenderGraph.h>


class SSAOBlurPass : public RenderPass
{
public:
	void init(VulkanCore* vulkanCore, ResourceManager* resourceManager, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat) override;
	void cleanup() override;

	void render(RenderContext& ctx, FrameData& frameData, DrawContext& drawCtx, const GPUSceneData& data, const SSAOBlurPassParameters& params, const RenderGraph& graph);

private:
	VulkanCore* _vulkanCore;
	VkDescriptorSetLayout _descriptorLayout;
	GraphicsPipeline _pipeline;
};