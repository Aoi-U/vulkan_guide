#pragma once

#include <core/layer/ILayer.h>
#include <core/Events.h>
#include <scene/Scene.h>
#include <filesystem>
#include <string>
#include <layer/EngineContext.h>

class Camera;

class SceneLayer : public ILayer
{
public:
	explicit SceneLayer(Camera* activeCamera = nullptr)
		: ILayer("SceneLayer"), _camera(activeCamera) {}

	~SceneLayer() override = default;

	void on_attach(EngineContext& ctx) override;
	void on_detach() override;

	void on_update(float deltaTime) override;
	void on_event(EngineEvent& event) override;

	void new_scene(const std::string& name = "Untitled Scene");
	bool save_scene(const std::filesystem::path& filePath = "");
	bool save_scene_as(const std::filesystem::path& newFilePath, const std::string& newSceneName = "");
	bool load_scene(const std::filesystem::path& filePath);

	void sync_environment_from_renderer();

	// play/stop 
	void begin_play();
	void end_play();
	bool is_playing() const noexcept { return _isPlayMode; }

	void set_camera(Camera* camera) noexcept { _camera = camera; }
	Camera* get_camera() const noexcept { return _camera; }

	Scene& get_scene() noexcept { return _scene; }
	const Scene& get_scene() const noexcept { return _scene; }

private:
	bool update_transform_hierarchy_recursive(entt::registry& reg, entt::entity entity, const glm::mat4& parentWorld, bool parentDirty);
	void resolve_streamed_component_assets(entt::registry& reg);
	void update_camera_and_lighting(float deltaTime);
	void compute_shadow_cascades(const glm::mat4& cameraView, float fovRadians, float aspect, float nearPlane, float maxShadowDistance, const glm::vec3& lightDir);

	// event handlers
	void on_save(const Events::SceneSaveEvent& e);
	void on_load(const Events::SceneLoadEvent& e);
	void on_new_scene(const Events::SceneNewSceneEvent& e);

	Scene _scene;
	Camera* _camera{ nullptr };
	bool _isPlayMode{ false };
	std::string _playModeSnapshotYaml;
};