#include "app.h"

#include <core/Components.h>
#include <resources/AssetRegistry.h>
#include <resources/ResourceLayer.h>
#include <resources/ModelSpawner.h>
#include <graphics/RenderLayer.h>
#include <app/editor/EditorLayer.h>
#include <glm/gtc/matrix_transform.hpp>
#include <chrono>

void App::init(EngineRuntimeMode runtimeMode)
{
	_window.init("Vulkan Engine", 1600, 900);
	_vulkanCore.init(_window);
	_resourceManager.init(&_vulkanCore);

	DefaultResources::get().init(&_resourceManager);
	_renderer.init(&_window, &_vulkanCore, _dispatcher, &_resourceManager);

	_engineCtx.window = &_window;
	_engineCtx.vulkanCore = &_vulkanCore;
	_engineCtx.resourceManager = &_resourceManager;
	_engineCtx.assetRegistry = &AssetRegistry::get();
	_engineCtx.renderer = &_renderer;
	_engineCtx.eventDispatcher = &_dispatcher;
	_engineCtx.layerStack = &_layerStack;
	_engineCtx.runtimeMode = runtimeMode;

	// connect sdl input event and layers
	_layerStack.connect_dispatcher(_dispatcher);
	_layerStack.push_layer<ResourceLayer>(_engineCtx, "../../assets", true);
	_sceneLayer = _layerStack.push_layer<SceneLayer>(_engineCtx, &_camera);
	_layerStack.push_layer<RenderLayer>(_engineCtx);

	if (_engineCtx.is_editor_mode()) {
		_editorLayer = _layerStack.push_overlay<EditorLayer>(_engineCtx);
	}
}

void App::run()
{
	using Clock = std::chrono::high_resolution_clock;
	auto lastTime = Clock::now();
	float accumulator = 0.0f;
	constexpr float FIXED_TIMESTEP = 1.0f / 60.0f;

	while (!_window.should_close()) {
		auto currentTime = Clock::now();
		float deltaTime = std::chrono::duration<float>(currentTime - lastTime).count();
		lastTime = currentTime;
		deltaTime = std::min(deltaTime, 0.25f);

		_window.poll_events(_dispatcher);

		_engineCtx.frameNumber = _renderer.get_frame_number();
		_engineCtx.deltaTime = deltaTime;

		_layerStack.begin_frame(_engineCtx.frameNumber);

		// fixed update for physics/deterministic simulation steps
		accumulator += deltaTime;
		while (accumulator >= FIXED_TIMESTEP) {
			_layerStack.fixed_update(FIXED_TIMESTEP);
			accumulator -= FIXED_TIMESTEP;
		}

		_layerStack.update(deltaTime);

		_layerStack.end_frame();
	}
}

void App::cleanup()
{
	vkDeviceWaitIdle(_vulkanCore.get_device());

	_layerStack.disconnect_dispatcher(_dispatcher);
	_layerStack.clear();
	_editorLayer = nullptr;

	_renderer.cleanup(_dispatcher);
	DefaultResources::get().cleanup();
	_resourceManager.cleanup();
	_vulkanCore.cleanup();
	_window.cleanup();
}
