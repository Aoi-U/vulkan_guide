#include "app.h"
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <chrono>
#include <thread>
#include <utils/vk_initializers.h>
#include <imgui_impl_vulkan.h>
#include <glm/gtx/transform.hpp>
#include <core/cvars.h>
#include <resources/Material.h>
#include <resources/Model.h>
#include <resources/AssetRegistry.h>
#include <resources/DefaultResources.h>

static void update_transform_hierarchy(entt::registry& registry, entt::entity entity, const glm::mat4& parentMatrix, bool parentDirty) {
	auto& tc = registry.get<TransformComponent>(entity);

	bool nodeDirty = tc.isDirty || parentDirty;
	// recalculate world transform  if dirty
	if (nodeDirty) {
		tc.worldTransform = parentMatrix * tc.get_transform();
		tc.isDirty = false;
	}

	if (auto* hc = registry.try_get<HierarchyComponent>(entity)) {
		for (auto child : hc->children) {
			update_transform_hierarchy(registry, child, tc.worldTransform, nodeDirty);
		}
	}
}

App::App()
{
	_window = std::make_unique<Window>();
	_vulkanCore = std::make_unique<VulkanCore>();
	_renderer = std::make_unique<Renderer>();
	_editor = std::make_unique<Editor>();
	_guiRenderer = std::make_unique<GuiRenderer>();
}

void App::init()
{
	auto& dispatcher = _registry.ctx().emplace<entt::dispatcher>();

	_window->init("Vulkan Engine", 1280, 720);
	_vulkanCore->init(*_window);

	_resourceManager = std::make_unique<ResourceManager>();
	_resourceManager->init(_vulkanCore.get());
	DefaultResources::get().init(_resourceManager.get());

	_renderer->init(_window.get(), _vulkanCore.get(), dispatcher, _resourceManager.get());
	_guiRenderer->init(_window.get(), _vulkanCore.get(), _renderer->get_swapchain_format());
	_renderer->set_gui_renderer(_guiRenderer.get());

	_registry.ctx().emplace<entt::dispatcher>();

	dispatcher.sink<Events::WindowCloseEvent>().connect<&App::on_window_close>(this);
	dispatcher.sink<Events::SDLInputEvent>().connect<&Editor::on_input_event>(_editor.get());
	dispatcher.sink<Events::SDLInputEvent>().connect<&Camera::on_input_event>(&_camera);
	dispatcher.sink<Events::LoadEnvironmentEvent>().connect<&App::on_load_environment>(this);
	dispatcher.sink<Events::ResetEnvironmentEvent>().connect<&App::on_reset_environment>(this);

	AssetRegistry::get().scan_directory("../../assets");	

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

		_camera.isViewportHovered = _editor->is_viewport_hovered();

		_window->poll_events(dispatcher);

		// skip rendering when the window is minimized
		if (_window->is_minimized()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}

		update_scene(_deltaTime);

		EngineStats stats;
		stats.frameTimeMs = _frameTime;
		stats.fps = 1.0f / _deltaTime;
		stats.renderStats = _renderer->get_stats();

		_guiRenderer->new_frame();
		_editor->draw_gui(_registry, *_resourceManager, stats, _sceneData, *_renderer);

		_renderer->render_frame(_registry, _sceneData);
	}
}

void App::cleanup()
{
	vkDeviceWaitIdle(_vulkanCore->get_device());

	auto& dispatcher = _registry.ctx().get<entt::dispatcher>();
	dispatcher.disconnect(this);
	_guiRenderer->cleanup();
	_registry.clear();
	DefaultResources::get().cleanup();
	_renderer->cleanup(dispatcher);
	_resourceManager->cleanup();
	_vulkanCore->cleanup();
	_window->cleanup();
}

void App::on_window_close(const Events::WindowCloseEvent& event)
{}

void App::on_load_environment(const Events::LoadEnvironmentEvent & event)
{
	vkDeviceWaitIdle(_vulkanCore->get_device());
	_renderer->load_environment(event.path);
}

void App::on_reset_environment(const Events::ResetEnvironmentEvent & event)
{
	vkDeviceWaitIdle(_vulkanCore->get_device());
	_renderer->remove_environment();
}

void App::update_scene(float deltaTime) {
	_camera.update(deltaTime);

	_resourceManager->process_main_thread_tasks(); // process any resource loading tasks that need to be done on the main thread

	_resourceManager->unload_unused();

	auto view = _registry.view<TransformComponent>();
	for (auto entity : view) {
		auto* hc = _registry.try_get<HierarchyComponent>(entity);
		if (!hc || hc->parent == entt::null) { 
			update_transform_hierarchy(_registry, entity, glm::mat4(1.0f), false);
		}
	}

	VkExtent2D extent = _renderer->get_viewport_extent();
	if (extent.width == 0 || extent.height == 0) {
		extent = _window->get_extent();
	}
	float aspect = (float)extent.width / (float)extent.height;

	_sceneData.proj = glm::perspective(glm::radians(70.0f), aspect, 10000.f, 0.1f);
	_sceneData.proj[1][1] *= -1;
	_sceneData.view = _camera.getViewMatrix();
	_sceneData.viewproj = _sceneData.proj * _sceneData.view;
	_sceneData.cameraPosition = glm::vec4(_camera.position, 1.0f);

	_sceneData.ambientColor = glm::vec4(0.03f, 0.03f, 0.03f, 1.0f);

	// default fallback directional light values
	_sceneData.sunlightColor = glm::vec4(0.0f);
	_sceneData.sunlightDirection = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
	_sceneData.sunlightViewproj = glm::mat4(1.0f);

	// directional light
	auto dirLightView = _registry.view<const DirectionalLightComponent>();
	for (auto [entity, dirLight] : dirLightView.each()) {
		if (!dirLight.visible) continue;

		_sceneData.sunlightColor = glm::vec4(dirLight.color, 1.0f);
		_sceneData.sunlightDirection = glm::vec4(glm::normalize(dirLight.direction), dirLight.intensity);

		glm::vec3 lightDir = glm::normalize(dirLight.direction);
		glm::vec3 lightPos = _camera.position + lightDir * dirLight.shadowExtent;
		glm::mat4 lightView = glm::lookAt(lightPos, _camera.position, glm::vec3(0.0f, 1.0f, 0.0f));
		float orthoSize = dirLight.shadowExtent;
		glm::mat4 lightProj = glm::ortho(-orthoSize, orthoSize, -orthoSize, orthoSize, 0.1f, 80.0f);
		lightProj[1][1] *= -1.0f;
		_sceneData.sunlightViewproj = lightProj * lightView;

		break; // Primary sun light
	}

	// point lights
	uint32_t activePointLights = 0;
	int shadowPointLightIndex = -1;
	float shadowFarPlane = 1.0f;

	auto pointLightView = _registry.view<const PointLightComponent>();
	for (auto [entity, pl] : pointLightView.each()) {
		if (!pl.visible) continue;
		if (activePointLights >= MAX_POINT_LIGHTS) break;

		glm::vec3 pos(0.0f);
		if (const auto* tc = _registry.try_get<TransformComponent>(entity)) {
			pos = tc->position;
		}

		_sceneData.pointLights[activePointLights].position = glm::vec4(pos, pl.radius);
		_sceneData.pointLights[activePointLights].color = glm::vec4(pl.color, pl.intensity);
		_sceneData.pointLights[activePointLights].params = glm::vec4(pl.castShadow ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);

		if (pl.castShadow && shadowPointLightIndex < 0) {
			shadowPointLightIndex = static_cast<int>(activePointLights);
			shadowFarPlane = pl.radius;
		}

		activePointLights++;
	}
	_sceneData.lightCount = glm::vec4(static_cast<float>(activePointLights), static_cast<float>(shadowPointLightIndex), shadowFarPlane, 0.0f);
}