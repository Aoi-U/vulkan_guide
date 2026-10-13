#pragma once

#include <graphics/pass/RenderPass.h>
#include <graphics/Pipeline.h>
#include <graphics/RenderGraph.h>

class ShadowPass : public RenderPass
{
public:
	void init(VulkanCore* vulkanCore, ResourceManager* resourceManager, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat) override;
	void cleanup() override;

	void render(RenderContext& ctx, FrameData& frameData, DrawContext& drawCtx, const GPUSceneData& sceneData, const ShadowPassParameters& params, const RenderGraph& graph);

private:
	VulkanCore* _vulkanCore{ nullptr };
	ResourceManager* _resourceManager{ nullptr };
	GraphicsPipeline _pipeline;
};