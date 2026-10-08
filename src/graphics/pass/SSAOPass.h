#pragma once

#include <graphics/pass/RenderPass.h>
#include <graphics/Pipeline.h>
#include <graphics/VulkanCore.h>
#include <resources/Buffer.h>
#include <resources/Sampler.h>

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
	void init(VulkanCore* vulkanCore, ResourceManager* resourceManager, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat) override;
	void cleanup() override;
	void render(RenderContext& ctx, FrameData& frameData, const std::unordered_map<std::string, ResourceHandle<Texture>> renderTargets, DrawContext& drawCtx, const GPUSceneData& sceneData) override;

private:
	void generate_kernal_and_noise();

	VulkanCore* _vulkanCore;
	ResourceManager* _resourceManager;
	VkDescriptorSetLayout _descriptorLayout;
	GraphicsPipeline _pipeline;

	ResourceHandle<Buffer> _kernelBuffer;
	ResourceHandle<Texture> _noiseImage;
	ResourceHandle<Sampler> _noiseSampler;
};