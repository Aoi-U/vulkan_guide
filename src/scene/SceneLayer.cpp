#include "SceneLayer.h"
#include <core/cvars.h>
#include <resources/AssetRegistry.h>
#include <resources/AssetSerializer.h>
#include <resources/DefaultResources.h>
#include <resources/ResourceManager.h>
#include <graphics/Renderer.h>
#include <window/window.h>
#include <glm/gtc/matrix_transform.hpp>
#include <fmt/core.h>
#include <array>
#include <limits>
#include <cmath>

void SceneLayer::on_attach(EngineContext& ctx)
{
	ILayer::on_attach(ctx);

	_ctx->activeScene = &_scene;
	_ctx->activeRegistry = &_scene.get_registry();
	_ctx->activeSceneData = &_scene.get_scene_data();

	// init default camera/lighting
	GPUSceneData& sd = _scene.get_scene_data();
	sd.cameraPosition = glm::vec4(0.0f, 2.0f, 6.0f, 1.0f);
	sd.view = glm::lookAt(glm::vec3(sd.cameraPosition), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	sd.proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 1000.0f, 0.1f);
	sd.proj[1][1] *= -1.0f;
	sd.viewproj = sd.proj * sd.view;
	sd.ambientColor = _scene.get_environment_settings().ambientColor;
	sd.sunlightDirection = _scene.get_environment_settings().sunlightDirection;
	sd.sunlightColor = _scene.get_environment_settings().sunlightColor;

	if (_ctx->eventDispatcher) {
		_ctx->eventDispatcher->sink<Events::SceneSaveEvent>().connect<&SceneLayer::on_save>(this);
		_ctx->eventDispatcher->sink<Events::SceneLoadEvent>().connect<&SceneLayer::on_load>(this);
		_ctx->eventDispatcher->sink<Events::SceneNewSceneEvent>().connect<&SceneLayer::on_new_scene>(this);
	}
}

void SceneLayer::on_detach()
{
	if (_ctx && _ctx->eventDispatcher) {
		_ctx->eventDispatcher->sink<Events::SceneSaveEvent>().disconnect(this);
		_ctx->eventDispatcher->sink<Events::SceneLoadEvent>().disconnect(this);
		_ctx->eventDispatcher->sink<Events::SceneNewSceneEvent>().disconnect(this);
	}

	_scene.clear();

	if (_ctx) {
		if (_ctx->activeScene == &_scene) {
			_ctx->activeScene = nullptr;
			_ctx->activeRegistry = nullptr;
			_ctx->activeSceneData = nullptr;
		}
	}

	ILayer::on_detach();
}

void SceneLayer::on_update(float deltaTime)
{
	if (!_ctx)
		return;

	entt::registry& reg = _scene.get_registry();
	resolve_streamed_component_assets(reg);

	// update dirty transforms
	bool anyTransformUpdated = false;
	auto transformView = reg.view<TransformComponent>();
	for (auto [entity, tc] : transformView.each()) {
		const auto* hc = reg.try_get<HierarchyComponent>(entity);
		if (!hc || hc->parent == entt::null || !reg.valid(hc->parent)) {
			anyTransformUpdated |= update_transform_hierarchy_recursive(reg, entity, glm::mat4(1.0f), false);
		}
	}

	if (anyTransformUpdated && _ctx->renderer) {
		_ctx->renderer->mark_scene_dirty();
	}

	update_camera_and_lighting(deltaTime);
}

void SceneLayer::on_event(EngineEvent & event)
{
	if (event.handled)
		return;

	if (event.sdlEvent.type == SDL_KEYDOWN && !event.sdlEvent.key.repeat) {
		const SDL_Keymod mods = static_cast<SDL_Keymod>(event.sdlEvent.key.keysym.mod);
		const bool ctrlPressed = (mods & KMOD_CTRL) != 0;
		const bool shiftPressed = (mods & KMOD_SHIFT) != 0;

		if (ctrlPressed && shiftPressed && event.sdlEvent.key.keysym.sym == SDLK_s) // Ctrl + Shift + S save scene as
		{
			if (_ctx && _ctx->eventDispatcher)
			{
				_ctx->eventDispatcher->trigger<Events::SceneSaveAsEvent>({});
			}
			event.handled = true;
		}
		else if (ctrlPressed && event.sdlEvent.key.keysym.sym == SDLK_s)
		{
			// Ctrl + S save to current scene
			save_scene();
			event.handled = true;
		}
		else if (ctrlPressed && event.sdlEvent.key.keysym.sym == SDLK_o)
		{
			// Ctrl + O reload current scene file path
			load_scene(_scene.get_file_path());
			event.handled = true;
		}
		else if (ctrlPressed && event.sdlEvent.key.keysym.sym == SDLK_n)
		{
			// Ctrl + N create new empty scene
			new_scene();
			event.handled = true;
		}
	}
}

void SceneLayer::new_scene(const std::string & name)
{
	_scene.clear();
	_scene.get_environment_settings().sceneUUID = UUID::generate();
	_scene.get_environment_settings().sceneName = name;
	_scene.set_file_path("../../assets/scenes/untitled.scene");

	// spawn default DirectionalLight entity 
	entt::entity sun = _scene.create_entity("Directional Sun");
	auto& dl = _scene.get_registry().emplace<DirectionalLightComponent>(sun);
	dl.direction = glm::vec3(0.4f, 1.0f, 0.3f);
	dl.color = glm::vec3(1.0f, 0.95f, 0.85f);
	dl.intensity = 3.0f;
	dl.shadowExtent = 35.0f;

	if (_ctx && _ctx->renderer) {
		_ctx->renderer->remove_environment();
		_ctx->renderer->mark_scene_dirty();
	}

	if (_ctx && _ctx->resourceManager) {
		_ctx->resourceManager->process_evictions_immediate();
	}

	fmt::println("SceneLayer: Created new scene '{}' (zero-ref GPU resources evicted)", name);
}

bool SceneLayer::save_scene(const std::filesystem::path & filePath)
{
	std::filesystem::path targetPath = filePath.empty() ? _scene.get_file_path() : filePath;
	if (targetPath.empty()) {
		targetPath = "../../assets/scenes/default.scene";
	}
	_scene.set_file_path(targetPath);

	sync_environment_from_renderer();

	auto& env = _scene.get_environment_settings();
	env.exposure = static_cast<float>(CVarSystem::Get()->GetFloatCVar("r.exposure"));
	env.bloomStrength = static_cast<float>(CVarSystem::Get()->GetFloatCVar("r.bloom.strength"));
	env.enableSSAO = (CVarSystem::Get()->GetIntCVar("r.ssao") != 0);

	bool ok = SceneSerializer::save_scene(targetPath, _scene.get_registry(), env);
	if (ok) {
		fmt::println("SceneLayer: Saved scene '{}' to '{}'", env.sceneName, targetPath.generic_string());
	}
	return ok;
}

bool SceneLayer::save_scene_as(const std::filesystem::path& newFilePath, const std::string& newSceneName)
{
	if (newFilePath.empty())
		return false;

	std::filesystem::path finalPath = newFilePath;
	if (finalPath.extension() != ".scene")
	{
		finalPath.replace_extension(".scene");
	}

	std::error_code ec;
	bool isDifferentFile = !std::filesystem::exists(finalPath, ec) || !std::filesystem::equivalent(
		finalPath, _scene.get_file_path(), ec);
	if (isDifferentFile)
	{
		_scene.get_environment_settings().sceneUUID = UUID::generate();
	}

	if (!newSceneName.empty())
	{
		_scene.get_environment_settings().sceneName = newSceneName;
	}
	else
	{
		_scene.get_environment_settings().sceneName = finalPath.stem().string();
	}

	bool saved = save_scene(finalPath);
	if (saved)
	{
		AssetRegistry::get().scan_directory("../../assets");
	}
	return saved;
}

bool SceneLayer::load_scene(const std::filesystem::path& filePath)
{
	if (!_ctx || !std::filesystem::exists(filePath)) {
		fmt::println("SceneLayer: cannot load scene: {}", filePath.string());
		return false;
	}

	ResourceManager* rm = _ctx->resourceManager;
	SceneAssetManifest incomingAssets{};
	SceneSerializer::extract_referenced_assets(filePath, incomingAssets);

	std::vector<ResourceHandle<Mesh>> pinnedMeshes;
	std::vector<ResourceHandle<Texture>> pinnedTextures;

	if (rm) {
		for (const UUID& meshUuid : incomingAssets.meshUUIDs) {
			if (rm->has_resource<Mesh>(meshUuid)) {
				pinnedMeshes.push_back(rm->get_handle<Mesh>(meshUuid));
			}
		}
		for (const UUID& texUuid : incomingAssets.textureUUIDs) {
			if (rm->has_resource<Texture>(texUuid)) {
				pinnedTextures.push_back(rm->get_handle<Texture>(texUuid));
			}
		}
	}

	bool loaded = SceneSerializer::load_scene(
		filePath,
		_scene.get_registry(),
		_scene.get_environment_settings(),
		rm,
		_ctx->renderer
	);

	if (loaded) {
		_scene.set_file_path(filePath);
	}

	pinnedMeshes.clear();
	pinnedTextures.clear();

	if (rm) {
		rm->process_evictions_immediate();
	}

	fmt::println("SceneLayer: loaded scene '{}'", filePath.generic_string());
	return loaded;
}

void SceneLayer::sync_environment_from_renderer()
{
	if (!_ctx || !_ctx->renderer)
		return;

	auto& env = _scene.get_environment_settings();
	if (_ctx->renderer->is_using_hdr_environment()) {
		std::filesystem::path hdrPath = _ctx->renderer->get_environment_path();
		UUID hdrUuid = _ctx->renderer->get_environment_hdr_uuid();

		env.useHDRSky = true;
		env.hdrEnvironmentPath = hdrPath;

		if (hdrUuid.is_valid()) {
			env.hdrEnvironmentUUID = hdrUuid;
		}
		else if (!hdrPath.empty()) {
			env.hdrEnvironmentUUID = AssetRegistry::get().resolve_or_derive_uuid(hdrPath.string());
		}
	}
	else {
		env.useHDRSky = false;
		env.hdrEnvironmentUUID = UUID::null();
		env.hdrEnvironmentPath.clear();
	}
}

void SceneLayer::begin_play()
{
	if (_isPlayMode)
		return;

	sync_environment_from_renderer();

	_playModeSnapshotYaml = SceneSerializer::serialize_to_string(_scene.get_registry(), _scene.get_environment_settings());
	_isPlayMode = true;
	fmt::println("SceneLayer: entered play mode");
}

void SceneLayer::end_play()
{
	if (!_isPlayMode)
		return;

	_isPlayMode = false;

	if (!_playModeSnapshotYaml.empty() && _ctx) {
		SceneSerializer::deserialize_from_string(_playModeSnapshotYaml, _scene.get_registry(), _scene.get_environment_settings(), _ctx->resourceManager, _ctx->renderer);
		_playModeSnapshotYaml.clear();
		fmt::println("SceneLayer: exited play mode");
	}
}

bool SceneLayer::update_transform_hierarchy_recursive(entt::registry& reg, entt::entity entity, const glm::mat4& parentWorld, bool parentDirty)
{
	if (!reg.valid(entity) || !reg.all_of<TransformComponent>(entity)) return false;

	auto& tc = reg.get<TransformComponent>(entity);
	bool dirty = tc.isDirty || parentDirty;
	if (dirty) {
		tc.worldTransform = parentWorld * tc.get_transform();
		tc.isDirty = false;
	}

	bool subtreeUpdated = dirty;
	if (const auto* hc = reg.try_get<HierarchyComponent>(entity)) {
		for (entt::entity child : hc->children) {
			subtreeUpdated |= update_transform_hierarchy_recursive(reg, child, tc.worldTransform, dirty);
		}
	}
	return subtreeUpdated;
}

void SceneLayer::resolve_streamed_component_assets(entt::registry& reg)
{
	if (!_ctx || !_ctx->resourceManager) 
		return;
	ResourceManager& rm = *_ctx->resourceManager;
	bool sceneDirtied = false;

	// resolve meshes that were queued from AssetStreamer or assigned by UUID
	auto meshView = reg.view<MeshComponent>();
	for (auto [entity, mc] : meshView.each()) {
		if (mc.meshAssetUUID.is_valid() && !mc.mesh.is_valid()) {
			if (rm.has_resource<Mesh>(mc.meshAssetUUID)) {
				mc.mesh = rm.get_handle<Mesh>(mc.meshAssetUUID);
				mc.bounds = mc.mesh->get_bounds();
				mc.startIndex = 0;
				mc.count = mc.mesh->get_index_count(0);
				sceneDirtied = true;
			}
			else {
				rm.get_or_request<Mesh>(mc.meshAssetUUID);
			}
		}
	}

	// resolve materials assigned by UUID
	auto matView = reg.view<MaterialComponent>();
	for (auto [entity, matc] : matView.each()) {
		if (matc.materialAssetUUID.is_valid() && !matc.material) {
			if (const auto* meta = AssetRegistry::get().get_metadata(matc.materialAssetUUID)) {
				std::filesystem::path matPath = meta->virtualPath;
				if (matPath.extension() != ".mat") {
					matPath = meta->virtualPath.parent_path() / "materials" / (meta->name + ".mat");
				}
				MaterialAssetData matData{};
				if (AssetSerializer::load_material_file(matPath, matData)) {
					matc.material = AssetSerializer::build_material_instance(matData, rm);
					matc.alphaMode = matData.alphaMode;
					matc.doubleSided = matData.doubleSided;
					matc.isDirty = true;
					sceneDirtied = true;
				}
			}
		}
	}

	if (sceneDirtied && _ctx->renderer) {
		_ctx->renderer->mark_scene_dirty();
	}
}

void SceneLayer::update_camera_and_lighting(float deltaTime)
{
	sync_environment_from_renderer();
	
	entt::registry& reg = _scene.get_registry();
	GPUSceneData& sceneData = _scene.get_scene_data();
	SceneEnvironmentSettings& env = _scene.get_environment_settings();

	sceneData.ambientColor = env.ambientColor;

	// active PointLightComponents
	uint32_t lightIdx = 0;
	int shadowCasterIdx = -1;
	auto pointLightView = reg.view<const TransformComponent, const PointLightComponent>();
	for (auto [entity, tc, pl] : pointLightView.each()) {
		if (!pl.visible || lightIdx >= MAX_POINT_LIGHTS) continue;

		glm::vec3 worldPos = glm::vec3(tc.worldTransform[3]);
		sceneData.pointLights[lightIdx].position = glm::vec4(worldPos, pl.radius);
		sceneData.pointLights[lightIdx].color = glm::vec4(pl.color, pl.intensity);
		sceneData.pointLights[lightIdx].params = glm::vec4(pl.castShadow ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);

		if (pl.castShadow && shadowCasterIdx < 0) {
			shadowCasterIdx = static_cast<int>(lightIdx);
		}
		lightIdx++;
	}
	sceneData.lightCount = glm::vec4(static_cast<float>(lightIdx), static_cast<float>(shadowCasterIdx), 0.0f, 0.0f);

	// DirectionalLightComponent and compute cascade shadow matrices
	glm::vec3 sunDir = glm::normalize(glm::vec3(env.sunlightDirection));
	float sunPower = env.sunlightDirection.w;
	glm::vec3 sunCol = glm::vec3(env.sunlightColor);
	float maxShadowExtent = 60.0f;

	auto dirLightView = reg.view<const DirectionalLightComponent>();
	for (auto [entity, dl] : dirLightView.each()) {
		if (!dl.visible) continue;
		if (glm::length(dl.direction) > 1e-4f) {
			sunDir = glm::normalize(dl.direction);
		}
		sunPower = dl.intensity;
		sunCol = dl.color;
		maxShadowExtent = std::max(10.0f, dl.shadowExtent * 2.0f);
		break;
	}

	sceneData.sunlightDirection = glm::vec4(sunDir, sunPower);
	sceneData.sunlightColor = glm::vec4(sunCol, 1.0f);
	env.sunlightDirection = sceneData.sunlightDirection;
	env.sunlightColor = sceneData.sunlightColor;

	VkExtent2D vpExtent = (_ctx && _ctx->renderer) ? _ctx->renderer->get_viewport_extent() : VkExtent2D{ 1280, 720 };
	float aspect = (vpExtent.height > 0)
		? (static_cast<float>(vpExtent.width) / static_cast<float>(vpExtent.height))
		: (16.0f / 9.0f);

	compute_shadow_cascades(sceneData.view, glm::radians(60.0f), aspect, 0.1f, maxShadowExtent, sunDir);
}

void SceneLayer::compute_shadow_cascades(const glm::mat4 & cameraView, float fovRadians, float aspect, float nearPlane, float maxShadowDistance, const glm::vec3 & lightDir)
{
	GPUSceneData& sceneData = _scene.get_scene_data();
	constexpr float lambda = 0.65f; 
	std::array<float, SHADOW_CASCADE_COUNT> splits{};

	float clipRange = maxShadowDistance - nearPlane;
	float minZ = nearPlane;
	float maxZ = nearPlane + clipRange;
	float range = maxZ - minZ;
	float ratio = maxZ / minZ;

	for (uint32_t i = 0; i < SHADOW_CASCADE_COUNT; ++i) {
		float p = static_cast<float>(i + 1) / static_cast<float>(SHADOW_CASCADE_COUNT);
		float logSplit = minZ * std::pow(ratio, p);
		float uniformSplit = minZ + range * p;
		//float d = lambda * (logSplit - uniformSplit) + uniformSplit;
		float d = std::lerp(uniformSplit, logSplit, lambda);
		splits[i] = d;
	}

	sceneData.cascadeSplits = glm::vec4(splits[0], splits[1], splits[2], splits[3]);

	glm::mat4 invCamView = glm::inverse(cameraView);
	glm::vec3 up = (std::abs(lightDir.y) > 0.99f) ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);

	float lastSplitDist = nearPlane;
	for (uint32_t c = 0; c < SHADOW_CASCADE_COUNT; ++c) {
		float splitDist = splits[c];

		// perspective slice for frustum corner extraction
		glm::mat4 sliceProj = glm::perspectiveRH_ZO(fovRadians, aspect, lastSplitDist, splitDist);
		sliceProj[1][1] *= -1.0f;
		glm::mat4 invSliceViewProj = invCamView * glm::inverse(sliceProj);

		std::array<glm::vec3, 8> frustumCorners = {
			glm::vec3(-1.0f,  1.0f, 0.0f),
			glm::vec3(1.0f,  1.0f, 0.0f),
			glm::vec3(1.0f, -1.0f, 0.0f),
			glm::vec3(-1.0f, -1.0f, 0.0f),
			glm::vec3(-1.0f,  1.0f, 1.0f),
			glm::vec3(1.0f,  1.0f, 1.0f),
			glm::vec3(1.0f, -1.0f, 1.0f),
			glm::vec3(-1.0f, -1.0f, 1.0f)
		};

		glm::vec3 center(0.0f);
		for (auto& corner : frustumCorners) {
			glm::vec4 worldCorner = invSliceViewProj * glm::vec4(corner, 1.0f);
			corner = glm::vec3(worldCorner) / worldCorner.w;
			center += corner;
		}
		center /= 8.0f;

		float radius = 0.0f;
		for (const auto& corner : frustumCorners) {
			radius = std::max(radius, glm::length(corner - center));
		}
		radius = std::ceil(radius * 16.0f) / 16.0f;

		glm::vec3 maxExtents(radius);
		glm::vec3 minExtents = -maxExtents;

		glm::mat4 lightView = glm::lookAt(center + lightDir * radius * 2.0f, center, up);
		glm::mat4 lightOrtho = glm::orthoRH_ZO(
			minExtents.x, maxExtents.x,
			minExtents.y, maxExtents.y,
			0.0f, maxExtents.z - minExtents.z + radius * 2.0f
		);
		lightOrtho[1][1] *= -1.0f;

		// texel snap 
		glm::mat4 shadowMatrix = lightOrtho * lightView;
		glm::vec4 shadowOrigin = shadowMatrix * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
		shadowOrigin *= (2048.0f / 2.0f);
		glm::vec4 roundedOrigin = glm::round(shadowOrigin);
		glm::vec4 roundOffset = (roundedOrigin - shadowOrigin) * (2.0f / 2048.0f);
		roundOffset.z = 0.0f;
		roundOffset.w = 0.0f;
		lightOrtho[3] += roundOffset;

		sceneData.cascadeViewproj[c] = lightOrtho * lightView;
		lastSplitDist = splitDist;
	}

	sceneData.sunlightViewproj = sceneData.cascadeViewproj[0];
}

void SceneLayer::on_save(const Events::SceneSaveEvent & e)
{
	save_scene(e.filePath);
}

void SceneLayer::on_load(const Events::SceneLoadEvent & e)
{
	load_scene(e.filePath);
}

void SceneLayer::on_new_scene(const Events::SceneNewSceneEvent & e)
{
	new_scene();
}
