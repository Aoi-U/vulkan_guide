#pragma once

#include <core/UUID.h>
#include <core/Components.h>
#include <graphics/utils/vk_types.h>
#include <scene/SceneSerializer.h>
#include <entt.hpp>
#include <filesystem>
#include <string>
#include <unordered_set>

struct SceneAssetManifest
{
	std::unordered_set<UUID> meshUUIDs;
	std::unordered_set<UUID> materialUUIDs;
	std::unordered_set<UUID> textureUUIDs;
	UUID hdrEnvironmentUUID{};
};

class Scene
{
public:
	Scene() = default;
	~Scene() = default;

	void clear(); 

	entt::entity create_entity(const std::string& tag = "New Entity", const UUID& explicitId = UUID::null());

	void destroy_entity(entt::entity entity);

	entt::entity duplicate_entity(entt::entity sourceEntity, entt::entity newParent = entt::null);

	void set_parent(entt::entity child, entt::entity parent);

	entt::entity get_entity_by_uuid(const UUID& uuid) const;

	SceneAssetManifest collect_referenced_assets() const;

	entt::registry& get_registry() noexcept { return _registry; }
	const entt::registry& get_registry() const noexcept { return _registry; }

	SceneEnvironmentSettings& get_environment_settings() noexcept { return _envSettings; }
	const SceneEnvironmentSettings& get_environment_settings() const noexcept { return _envSettings; }

	GPUSceneData& get_scene_data() noexcept { return _sceneData; }
	const GPUSceneData& get_scene_data() const noexcept{ return _sceneData; }

	const std::filesystem::path& get_file_path() const noexcept { return _filePath; }
	void set_file_path(std::filesystem::path path) { _filePath = std::move(path); }

private:
	entt::registry _registry;
	SceneEnvironmentSettings _envSettings{};
	GPUSceneData _sceneData{};
	std::filesystem::path _filePath{ "../../assets/scenes/default.scene" };
};