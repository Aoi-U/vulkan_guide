#pragma once

#include <graphics/pass/RenderFeature.h>
#include <resources/DefaultResources.h>

struct TransparentPassParams {
  RGBufferHandle gpuObjectBuffer;
  RGBufferHandle gpuInstanceBuffer;
  RGBufferHandle materialBuffer;
  RGBufferHandle vtDescBuffer;
  RGBufferHandle vtFeedbackBuffer;
  RGTextureHandle sceneColorCopy;
  RGTextureHandle irradianceMap;
  RGTextureHandle prefilterMap;
  RGTextureHandle brdfLut;
  RGTextureHandle sceneColor;
  RGTextureHandle sceneDepth;
};

class ForwardTransparentPass : public RenderFeature
{
public:
	ForwardTransparentPass()
		: RenderFeature("ForwardTransparentPass", true) {}

	void init(VulkanCore* core, ResourceManager* rm) override;

	void cleanup() override;

	void add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view) override;

  void on_imgui() override;

private:
  VulkanCore* _core{ nullptr };
  ResourceManager* _rm{ nullptr };
  ResourceHandle<MaterialTemplate> _forwardVfxTemplate;
  bool _captureSceneColorCopy{ true };
  BlendMode _defaultBlendMode{ BlendMode::AlphaBlend };
};