#include "App.h"
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <chrono>
#include <thread>

#include <utils/vk_initializers.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_vulkan.h>
#include <glm/gtx/transform.hpp>


App::App()
{
	_window = std::make_unique<Window>();
	_vulkanCore = std::make_unique<VulkanCore>();
	_materialSystem = std::make_unique<MaterialSystem>();
	_renderer = std::make_unique<Renderer>();
	_editor = std::make_unique<Editor>();
	_guiRenderer = std::make_unique<GuiRenderer>();
}

void App::init()
{

	_window->init("Vulkan Engine", 1280, 720);
	_vulkanCore->init(*_window);

	_materialSystem->init(_vulkanCore.get());

	_renderer->init(_window.get(), _vulkanCore.get(), _materialSystem.get());
	
	_assetManager.init(_vulkanCore.get(), _materialSystem.get());
	_guiRenderer->init(_window.get(), _vulkanCore.get(), _renderer->get_swapchain_format());

	auto structureAsset = _assetManager.loadScene("structure", "../../assets/structure.glb");
	auto brutalistAsset = _assetManager.loadScene("brutalist_building", "../../assets/brutalist_building.glb");

	_camera.position = glm::vec3(0.0f, 0.0f, 0.0f);
	_camera.pitch = 0.0f;
	_camera.yaw = 0.0f;
}

void App::run()
{
	auto lastTime = std::chrono::steady_clock::now();

	while (!_window->should_close()) {
		auto currentTime = std::chrono::steady_clock::now();
		_deltaTime = std::chrono::duration<float>(currentTime - lastTime).count();
		_frameTime = _deltaTime * 1000.f;
		lastTime = currentTime;

		_window->poll_events([&](const SDL_Event& e) {
			_editor->process_event(e);
			if (!_editor->want_capture_mouse() || _camera.isActive) {
				_camera.processSDLEvent(const_cast<SDL_Event&>(e));
			}
			});

		// skip rendering when the window is minimized
		if (_window->is_minimized()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}

		EngineStats stats;
		stats.frameTimeMs = _frameTime;
		stats.fps = 1.0f / _deltaTime;
		stats.renderStats = _renderer->get_stats();

		_guiRenderer->new_frame();
		_editor->draw_gui(_registry, _assetManager, stats);

		update_scene(_deltaTime);

		_renderer->render_frame(_registry, _sceneData,
			[&](VkCommandBuffer cmd, VkImageView targetImageView, VkExtent2D extent) {
				_guiRenderer->render(cmd, targetImageView, extent);
			});
	}
}

void App::cleanup()
{
	vkDeviceWaitIdle(_vulkanCore->get_device());

	_guiRenderer->cleanup();
	_registry.clear();
	_assetManager.cleanup();
	_renderer->cleanup();
	_materialSystem->cleanup();
	_vulkanCore->cleanup();
	_window->cleanup();
}

void App::update_scene(float deltaTime) {
	_camera.update(deltaTime);

	VkExtent2D extent = _window->get_extent();
	_sceneData.proj = glm::perspective(glm::radians(70.0f) ,(float)extent.width / (float)extent.height, 10000.f, 0.1f);
	_sceneData.proj[1][1] *= -1;
	_sceneData.view = _camera.getViewMatrix();
	_sceneData.viewproj = _sceneData.proj * _sceneData.view;
	_sceneData.ambientColor = glm::vec4(0.1f);
	_sceneData.sunlightColor = glm::vec4(1.0f);
	_sceneData.sunlightDirection = glm::vec4(0.f, 1.f, 0.5f, 1.f);
}