#pragma once

#include <graphics/pass/RenderFeature.h>
#include <graphics/Pipeline.h>
#include <resources/Texture.h>
#include <unordered_map>
#include <variant>

struct CustomPostVFXGraphParams {
  RGTextureHandle sceneColorIn;
  RGTextureHandle sceneDepth;
  RGTextureHandle gPosition;
  RGTextureHandle gNormal;
  RGTextureHandle gAlbedo;
  RGTextureHandle gEmissive;
  RGTextureHandle sceneColorOut;
};

class CustomPostVFXPass : public RenderFeature
{
public:
  using ParamVariant = std::variant<float, int, glm::vec4>;

  CustomPostVFXPass(std::string passName, std::string fragShaderId, bool enabledByDefault = true);

  void init(VulkanCore* core, ResourceManager* rm) override;
  void cleanup() override;

  void add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view) override;

  void on_imgui() override;

  // runtime parameters for push constants and custom textures
  void set_float(const std::string & name, float value, float minValue = 0.0f, float maxValue = 1.0f);
  void set_int(const std::string& name, int value);
  void set_vec4(const std::string& name, const glm::vec4& value);
  void set_custom_texture(const std::string& bindingName, ResourceHandle<Texture> texture);

private:
  bool shader_uses_binding(const std::string& name) const;

  struct FloatParam
  {
    float minValue{ 0.0f };
    float maxValue{ 1.0f };
  };

  std::string _fragShaderId;
  VulkanCore* _core{ nullptr };
  ResourceManager* _rm{ nullptr };
  GraphicsPipeline _pipeline;

  std::unordered_map<std::string, ParamVariant> _parameters;
  std::unordered_map<std::string, FloatParam> _floatRanges;
  std::unordered_map<std::string, ResourceHandle<Texture>> _customTextures;
};