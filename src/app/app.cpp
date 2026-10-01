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
	auto& dispatcher = _registry.ctx().emplace<entt::dispatcher>();

	_window->init("Vulkan Engine", 1280, 720);
	_vulkanCore->init(*_window);

	_materialSystem->init(_vulkanCore.get());

	_renderer->init(_window.get(), _vulkanCore.get(), _materialSystem.get(), dispatcher);
	
	_assetManager.init(_vulkanCore.get(), _materialSystem.get());
	_guiRenderer->init(_window.get(), _vulkanCore.get(), _renderer->get_swapchain_format());

	_registry.ctx().emplace<entt::dispatcher>();

	dispatcher.sink<Events::WindowCloseEvent>().connect<&App::on_window_close>(this);
	dispatcher.sink<Events::SDLInputEvent>().connect<&Editor::on_input_event>(_editor.get());
	dispatcher.sink<Events::SDLInputEvent>().connect<&Camera::on_input_event>(&_camera);

	auto structureAsset = _assetManager.loadScene("structure", "../../assets/structure.glb");
	auto brutalistAsset = _assetManager.loadScene("brutalist_building", "../../assets/brutalist_building.glb");

	_camera.position = glm::vec3(0.0f, 0.0f, 0.0f);
	_camera.pitch = 0.0f;
	_camera.yaw = 0.0f;
}

void App::run()
{
	auto lastTime = std::chrono::steady_clock::now();
	auto& dispatcher = _registry.ctx().get<entt::dispatcher>();

	while (!_window->should_close()) {
		auto currentTime = std::chrono::steady_clock::now();
		_deltaTime = std::chrono::duration<float>(currentTime - lastTime).count();
		_frameTime = _deltaTime * 1000.f;
		lastTime = currentTime;

		_window->poll_events(dispatcher);

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

	auto& dispatcher = _registry.ctx().get<entt::dispatcher>();
	dispatcher.disconnect(this);

	_guiRenderer->cleanup();
	_registry.clear();
	_assetManager.cleanup();
	_renderer->cleanup(dispatcher);
	_materialSystem->cleanup();
	_vulkanCore->cleanup();
	_window->cleanup();
}

void App::on_window_close(const Events::WindowCloseEvent& event)
{}

void App::update_scene(float deltaTime) {
	_camera.update(deltaTime);

	VkExtent2D extent = _window->get_extent();
	_sceneData.proj = glm::perspective(glm::radians(70.0f) ,(float)extent.width / (float)extent.height, 10000.f, 0.1f);
	_sceneData.proj[1][1] *= -1;
	_sceneData.view = _camera.getViewMatrix();
	_sceneData.viewproj = _sceneData.proj * _sceneData.view;
	_sceneData.cameraPosition = glm::vec4(_camera.position, 1.0f);
	_sceneData.ambientColor = glm::vec4(0.1f);
	_sceneData.sunlightColor = glm::vec4(1.0f);
	_sceneData.sunlightDirection = glm::vec4(0.f, 1.f, 0.5f, 1.f);

	glm::vec3 lightDir = glm::normalize(glm::vec3(_sceneData.sunlightDirection));
	glm::vec3 lightPos = _camera.position + lightDir * 35.0f;
	glm::mat4 lightView = glm::lookAt(lightPos, _camera.position, glm::vec3(0.0f, 1.0f, 0.0f));
	float orthoSize = 35.0f;
	glm::mat4 lightProj = glm::ortho(-orthoSize, orthoSize, -orthoSize, orthoSize, 0.1f, 80.0f);
	lightProj[1][1] *= -1.0f;
	_sceneData.sunlightViewproj = lightProj * lightView;
}