#pragma once

#include <window/window.h>
#include <graphics/VulkanCore.h>
#include <graphics/Renderer.h>
#include <resources/ResourceManager.h>
#include <core/layer/EngineContext.h>
#include <core/layer/LayerStack.h>
#include <entt.hpp>
#include <cstdint>

class EditorLayer;

class App
{
public:
	App() = default;
	~App() = default;

	void init(EngineRuntimeMode runtimeMode = EngineRuntimeMode::Editor);
	void run(); // main loop
	void cleanup();

	EngineContext& get_engine_context() noexcept { return _engineCtx; }
	LayerStack& get_layer_stack() noexcept { return _layerStack; }

private:
  Window _window;
  VulkanCore _vulkanCore;
  ResourceManager _resourceManager;
  Renderer _renderer;

  entt::registry _registry;
  entt::dispatcher _dispatcher;
  GPUSceneData _sceneData{};

  EngineContext _engineCtx{};
  LayerStack _layerStack;
  EditorLayer* _editorLayer{ nullptr };
};