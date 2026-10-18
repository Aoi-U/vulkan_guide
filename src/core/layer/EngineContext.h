#pragma once

#include <entt.hpp>
#include <cstdint>

class Window;
class VulkanCore;
class ResourceManager;
class AssetRegistry;
class Renderer;
class Scene;
class LayerStack;
struct GPUSceneData;

enum class EngineRuntimeMode : uint8_t
{
	Editor,
	Standalone
};

struct EngineContext
{
  Window* window{ nullptr };
  VulkanCore* vulkanCore{ nullptr };

  ResourceManager* resourceManager{ nullptr };
  AssetRegistry* assetRegistry{ nullptr };

  Renderer* renderer{ nullptr };
  Scene* activeScene{ nullptr };

  entt::registry* activeRegistry{ nullptr };
  GPUSceneData* activeSceneData{ nullptr };

  entt::dispatcher* eventDispatcher{ nullptr };
  LayerStack* layerStack{ nullptr };

  EngineRuntimeMode runtimeMode{ EngineRuntimeMode::Editor };
  uint64_t frameNumber{ 0 };
  float deltaTime{ 0.0f };

  bool is_editor_mode() const noexcept { return runtimeMode == EngineRuntimeMode::Editor; }
  bool is_standalone_mode() const noexcept { return runtimeMode == EngineRuntimeMode::Standalone; }
};