#pragma once
#include <window/window.h>
//#include <graphics/vk_engine.h>
#include <graphics/VulkanCore.h>
#include <graphics/Renderer.h>
#include <resources/AssetManager.h>
#include <graphics/MaterialSystem.h>
#include <app/camera.h>
#include <memory>
#include <entt.hpp>
#include <core/Components.h>
#include <app/Editor.h>
#include <graphics/GuiRenderer.h>
#include <core/Events.h>

class App
{
public:
	App();

	void init();
	void run(); // main loop
	void cleanup();
	
	void on_window_close(const Events::WindowCloseEvent& event);
	void on_load_environment(const Events::LoadEnvironmentEvent& event);
	void on_reset_environment(const Events::ResetEnvironmentEvent& event);

private:
	void update_scene(float deltaTime);

	std::unique_ptr<Window> _window;
	std::unique_ptr<VulkanCore> _vulkanCore;
	std::unique_ptr<MaterialSystem> _materialSystem;
	std::unique_ptr<Renderer> _renderer;
	std::unique_ptr<Editor> _editor;
	std::unique_ptr<GuiRenderer> _guiRenderer;

	entt::registry _registry;

	AssetManager _assetManager;
	Camera _camera;
	GPUSceneData _sceneData;

	float _deltaTime{ 0.f };
	float _frameTime{ 0.f };
};