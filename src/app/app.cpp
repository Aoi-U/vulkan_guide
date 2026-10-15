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
	//AssetRegistry::get().print_manifest();

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

		_guiRenderer->new_frame();
		_editor->draw_gui(_registry, *_resourceManager, _sceneData, *_renderer);

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
		glm::vec3 lightDir = glm::normalize(dirLight.direction);
		_sceneData.sunlightDirection = glm::vec4(lightDir, dirLight.intensity);

		constexpr uint32_t CASCADE_COUNT = 4;
		constexpr float SHADOW_MAP_RES = 2048.0f;
		const float nearClip = 0.1f;
		const float maxShadowDist = std::max(dirLight.shadowExtent * 3.0f, 40.0f); 
		const float lambda = 0.85f; 

		std::array<float, CASCADE_COUNT + 1> splits{};
		splits[0] = nearClip;
		for (uint32_t i = 1; i <= CASCADE_COUNT; ++i) {
			float p = static_cast<float>(i) / static_cast<float>(CASCADE_COUNT);
			float logSplit = nearClip * std::pow(maxShadowDist / nearClip, p);
			float uniformSplit = nearClip + (maxShadowDist - nearClip) * p;
			splits[i] = lambda * logSplit + (1.0f - lambda) * uniformSplit;
		}

		_sceneData.cascadeSplits = glm::vec4(splits[1], splits[2], splits[3], splits[4]);

		glm::vec3 up = (std::abs(lightDir.y) > 0.99f) ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);

		for (uint32_t c = 0; c < CASCADE_COUNT; ++c) {
			float cNear = splits[c];
			float cFar = splits[c + 1];

			// standard perspective slice for [cNear, cFar] to extract 8 frustum slice corners
			glm::mat4 sliceProj = glm::perspective(glm::radians(70.0f), aspect, cNear, cFar);
			sliceProj[1][1] *= -1.0f;
			glm::mat4 invSliceViewProj = glm::inverse(sliceProj * _sceneData.view);

			std::array<glm::vec3, 8> frustumCorners{};
			uint32_t idx = 0;
			for (uint32_t x = 0; x < 2; ++x) {
				for (uint32_t y = 0; y < 2; ++y) {
					for (uint32_t z = 0; z < 2; ++z) {
						glm::vec4 pt = invSliceViewProj * glm::vec4(
							x ? 1.0f : -1.0f,
							y ? 1.0f : -1.0f,
							static_cast<float>(z),
							1.0f);
						frustumCorners[idx++] = glm::vec3(pt) / pt.w;
					}
				}
			}

			// compute frustum slice center and rotation invariant bounding sphere radius
			glm::vec3 center(0.0f);
			for (const auto& corner : frustumCorners) center += corner;
			center /= 8.0f;

			float radius = 0.0f;
			for (const auto& corner : frustumCorners) {
				radius = std::max(radius, glm::length(corner - center));
			}
			radius = std::ceil(radius * 16.0f) / 16.0f;

			// texel snap cascade center in light view space to eliminate shadow edge swimming
			glm::mat4 baseLightView = glm::lookAt(lightDir * radius, glm::vec3(0.0f), up);
			glm::vec4 lightSpaceCenter = baseLightView * glm::vec4(center, 1.0f);
			float texelSize = (radius * 2.0f) / SHADOW_MAP_RES;
			lightSpaceCenter.x = std::floor(lightSpaceCenter.x / texelSize) * texelSize;
			lightSpaceCenter.y = std::floor(lightSpaceCenter.y / texelSize) * texelSize;
			glm::vec3 snappedCenter = glm::vec3(glm::inverse(baseLightView) * lightSpaceCenter);

			glm::mat4 lightView = glm::lookAt(snappedCenter + lightDir * radius, snappedCenter, up);

			// pull zNear back (-radius * 6.0f) so tall objects outside the view frustum cast shadows into the slice
			float zPullback = std::max(radius * 6.0f, maxShadowDist);
			glm::mat4 lightProj = glm::ortho(-radius, radius, -radius, radius, -zPullback, radius * 2.0f);
			lightProj[1][1] *= -1.0f;

			_sceneData.cascadeViewproj[c] = lightProj * lightView;
		}

		_sceneData.sunlightViewproj = _sceneData.cascadeViewproj[0];
		break;
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