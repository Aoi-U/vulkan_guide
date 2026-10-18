#include "app.h"

#include <core/Components.h>
#include <resources/AssetRegistry.h>
#include <resources/ResourceLayer.h>
#include <resources/ModelSpawner.h>
#include <graphics/RenderLayer.h>
#include <app/editor/EditorLayer.h>
#include <glm/gtc/matrix_transform.hpp>
#include <chrono>

namespace
{
	class SceneLayer : public ILayer
	{
	public:
		SceneLayer()
			: ILayer("SceneLayer") {}

		void on_update(float deltaTime) override
		{
			if (!_ctx || !_ctx->activeRegistry || !_ctx->activeSceneData)
				return;

			entt::registry& reg = *_ctx->activeRegistry;
			GPUSceneData& sceneData = *_ctx->activeSceneData;

			VkExtent2D viewportExtent = _ctx->renderer->get_viewport_extent();
			float aspect = (viewportExtent.height > 0)
				? static_cast<float>(viewportExtent.width) / static_cast<float>(viewportExtent.height)
				: (16.0f / 9.0f);

			sceneData.proj = glm::perspective(glm::radians(60.0f), aspect, 1000.0f, 0.1f); // reverse z [1, 0]
			sceneData.proj[1][1] *= -1.0f; // vulkan y axis flip
			sceneData.viewproj = sceneData.proj * sceneData.view;

			// propagate dirty transform component hierarchies
			bool anyTransformUpdated = false;
			auto transformView = reg.view<TransformComponent>();
			for (auto [entity, tc] : transformView.each()) {
				const auto* hc = reg.try_get<HierarchyComponent>(entity);
				if (!hc || hc->parent == entt::null) {
					anyTransformUpdated |= update_hierarchy(reg, entity, glm::mat4(1.0f), false);
				}
			}

			if (anyTransformUpdated && _ctx->renderer) {
				_ctx->renderer->mark_scene_dirty();
			}

			// pack PointLightComponent into GPUSceneData
			uint32_t lightIdx = 0;
			int shadowCasterIdx = -1;
			auto pointLightView = reg.view<const TransformComponent, const PointLightComponent>();
			for (auto [entity, tc, pl] : pointLightView.each()) {
				if (!pl.visible || lightIdx >= MAX_POINT_LIGHTS)
					continue;

				auto worldPos = glm::vec3(tc.worldTransform[3]);
				sceneData.pointLights[lightIdx].position = glm::vec4(worldPos, pl.radius);
				sceneData.pointLights[lightIdx].color = glm::vec4(pl.color, pl.intensity);
				sceneData.pointLights[lightIdx].params = glm::vec4(pl.castShadow ? 1.0f : 0.0f, 0.0, 0.0f, 0.0f);

				if (pl.castShadow && shadowCasterIdx < 0) {
					shadowCasterIdx = static_cast<int>(lightIdx);
				}
				lightIdx++;
			}
			sceneData.lightCount = glm::vec4(static_cast<float>(lightIdx), static_cast<float>(shadowCasterIdx), 0.0f, 0.0f);

			// pack DirectionalLightComponent into GPUSceneData
			auto dirLightView = reg.view<const DirectionalLightComponent>();
			for (auto [entity, dl] : dirLightView.each()) {
				if (!dl.visible)
					continue;

				sceneData.sunlightDirection = glm::vec4(glm::normalize(dl.direction), dl.intensity);
				sceneData.sunlightColor = glm::vec4(dl.color, 1.0f);
				break;
			}
		}

	private:
		static bool update_hierarchy(entt::registry& reg, entt::entity entity, const glm::mat4& parentWorld, bool parentDirty)
		{
			if (!reg.valid(entity) || !reg.all_of<TransformComponent>(entity)) return false;

			auto& tc = reg.get<TransformComponent>(entity);
			bool dirty = tc.isDirty || parentDirty;
			// recalculate world transform if dirty
			if (dirty) {
				tc.worldTransform = parentWorld * tc.get_transform();
				tc.isDirty = false;
			}

			bool subtreeUpdated = dirty;
			if (const auto* hc = reg.try_get<HierarchyComponent>(entity)) {
				for (entt::entity child : hc->children) {
					subtreeUpdated |= update_hierarchy(reg, child, tc.worldTransform, dirty);
				}
			}
			return subtreeUpdated;
		}
	};
}

void App::init(EngineRuntimeMode runtimeMode)
{
	_window.init("Vulkan Engine", 1600, 900);
	_vulkanCore.init(_window);
	_resourceManager.init(&_vulkanCore);

	DefaultResources::get().init(&_resourceManager);
	_renderer.init(&_window, &_vulkanCore, _dispatcher, &_resourceManager);

	// default camera/scene uniform values
	_sceneData.cameraPosition = glm::vec4(0.0f, 2.0f, 6.0f, 1.0f);
	_sceneData.view = glm::lookAt(glm::vec3(_sceneData.cameraPosition), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	_sceneData.proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 1000.0f, 0.1f); // reverse z [1, 0]
	_sceneData.proj[1][1] *= -1.0f;
	_sceneData.viewproj = _sceneData.proj * _sceneData.view;
	_sceneData.ambientColor = glm::vec4(0.15f, 0.18f, 0.25f, 1.0f);
	_sceneData.sunlightDirection = glm::vec4(0.4f, 1.0f, 0.3f, 3.0f);
	_sceneData.sunlightColor = glm::vec4(1.0f, 0.95f, 0.85f, 1.0f);

	_engineCtx.window = &_window;
	_engineCtx.vulkanCore = &_vulkanCore;
	_engineCtx.resourceManager = &_resourceManager;
	_engineCtx.assetRegistry = &AssetRegistry::get();
	_engineCtx.renderer = &_renderer;
	_engineCtx.activeRegistry = &_registry;
	_engineCtx.activeSceneData = &_sceneData;
	_engineCtx.eventDispatcher = &_dispatcher;
	_engineCtx.layerStack = &_layerStack;
	_engineCtx.runtimeMode = runtimeMode;

	// connect sdl input event and layers
	_layerStack.connect_dispatcher(_dispatcher);
	_layerStack.push_layer<ResourceLayer>(_engineCtx, "../../assets", true);
	_layerStack.push_layer<SceneLayer>(_engineCtx);
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

	_registry.clear();
	_renderer.cleanup(_dispatcher);
	DefaultResources::get().cleanup();
	_resourceManager.cleanup();
	_vulkanCore.cleanup();
	_window.cleanup();
}
