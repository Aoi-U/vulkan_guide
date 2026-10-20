#pragma once

#include <window/window.h>
#include <graphics/VulkanCore.h>
#include <graphics/Renderer.h>
#include <resources/ResourceManager.h>
#include <core/layer/EngineContext.h>
#include <core/layer/LayerStack.h>
#include <scene/SceneLayer.h>
#include <app/camera.h>
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
  Camera _camera;

  entt::dispatcher _dispatcher;
  EngineContext _engineCtx{};
  LayerStack _layerStack;

  SceneLayer* _sceneLayer{ nullptr };
  EditorLayer* _editorLayer{ nullptr };
};