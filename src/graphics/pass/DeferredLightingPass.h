#pragma once

#include <graphics/pass/RenderFeature.h>
#include <graphics/Pipeline.h>
#include <graphics/RenderGraph.h>

struct DeferredLightingGraphParams {
  RGTextureHandle gPosition;
  RGTextureHandle gNormal;
  RGTextureHandle gAlbedo;
  RGTextureHandle gMetalRough;
  RGTextureHandle gEmissive;
  RGTextureHandle shadowMap;
  RGTextureHandle pointShadowMap;
  RGTextureHandle ssao;
  RGTextureHandle irradianceMap;
  RGTextureHandle prefilterMap;
  RGTextureHandle brdfLut;
  RGTextureHandle sceneColor;
  int ssaoEnabled{ 1 };
};

class DeferredLightingPass : public RenderFeature 
{
public:
  DeferredLightingPass()
    : RenderFeature("DeferredLightingPass", true) {}

  void init(VulkanCore* vulkanCore, ResourceManager* resourceManager) override;
  void cleanup() override;

  void add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view) override;

  void render(RenderContext& ctx, FrameData& frameData, DrawContext& drawCtx, const GPUSceneData& sceneData, const DeferredPassParameters& params, const RenderGraph& graph);

private:
  VulkanCore* _vulkanCore{ nullptr };
  GraphicsPipeline _pipeline;
};