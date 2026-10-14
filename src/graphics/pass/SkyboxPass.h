#pragma once

#include <graphics/pass/RenderFeature.h>
#include <graphics/VulkanCore.h>
#include <graphics/RenderGraph.h>

struct SkyboxGraphParams {
  RGTextureHandle envCubemap;
  RGTextureHandle sceneColor;
  RGTextureHandle sceneDepth;
};

class SkyboxPass : public RenderFeature
{
public:
  SkyboxPass()
    : RenderFeature("SkyboxPass", true) {};

  void init(VulkanCore* vulkanCore, ResourceManager* resourceManager) override;
  void cleanup() override;

  void add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view) override;

  void render(RenderContext& ctx, FrameData& frameData, DrawContext& drawCtx, const GPUSceneData& sceneData, const SkyboxPassParameters& params, const RenderGraph& graph);

private:
  VulkanCore* _vulkanCore{ nullptr };
  GraphicsPipeline _pipeline;
};