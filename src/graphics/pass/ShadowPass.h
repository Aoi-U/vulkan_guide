#pragma once

#include <graphics/pass/RenderPass.h>
#include <graphics/Pipeline.h>

class ShadowPass : public RenderPass
{
public:
	void init(VulkanCore* vulkanCore, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat) override;
	void cleanup() override;
	void render(RenderContext& ctx, FrameData& frameData, RenderResourceManager& resources, DrawContext& drawCtx, const GPUSceneData& sceneData) override;

private:
	VulkanCore* _vulkanCore;
	VkDescriptorSetLayout _descriptorLayout;
	GraphicsPipeline _pipeline;
};