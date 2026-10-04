#pragma once

#include <graphics/pass/RenderPass.h>
#include <graphics/Pipeline.h>
#include <graphics/VulkanCore.h>

struct SSAOPushConstants
{
	float radius{ 0.5 };
	float bias{ 0.025 };
	float power{ 1.5 };
	int kernelSize{ 64 };
};

class SSAOPass : public RenderPass
{
public:
	void init(VulkanCore* vulkanCore, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat) override;
	void cleanup() override;
	void render(RenderContext& ctx, FrameData& frameData, RenderResourceManager& resources, DrawContext& drawCtx, const GPUSceneData& sceneData) override;

private:
	void generate_kernal_and_noise();

	VulkanCore* _vulkanCore;
	VkDescriptorSetLayout _descriptorLayout;
	GraphicsPipeline _pipeline;

	AllocatedBuffer _kernelBuffer;
	AllocatedImage _noiseImage;
	VkSampler _noiseSampler;
};