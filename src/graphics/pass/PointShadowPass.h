#pragma once

#include <graphics/pass/RenderPass.h>
#include <graphics/Pipeline.h>
#include <graphics/RenderGraph.h>
#include <string>

struct PointShadowPushConstants
{
	glm::mat4 lightViewproj;
	glm::vec4 lightPos; // xyz: light position, w: far plane
};

class PointShadowPass : public RenderPass
{
public:
	void init(VulkanCore* vulkanCore, ResourceManager* resourceManager, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat) override;
	void cleanup() override;

	void render(RenderContext& ctx, FrameData& frameData, const std::unordered_map<std::string, ResourceHandle<Texture>>& renderTargets, const DrawContext& drawCtx, const GPUSceneData& sceneData, const PointShadowPassParameters& params, const RenderGraph& graph);

private:
	VulkanCore* _vulkanCore;
	VkDescriptorSetLayout _descriptorLayout;
	GraphicsPipeline _pipeline;
};