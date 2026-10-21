#pragma once

#include <core/UUID.h>
#include <entt.hpp>
#include <glm/glm.hpp>
#include <filesystem>
#include <string>

class ResourceManager;
class Renderer;
struct SceneAssetManifest;

struct SceneEnvironmentSettings
{
	UUID sceneUUID{ UUID::generate() };
	std::string sceneName{ "Untitled Scene" };
	bool useHDRSky{ false };
	UUID hdrEnvironmentUUID{};
	std::filesystem::path hdrEnvironmentPath{};
	
	glm::vec4 ambientColor{ 0.2f, 0.2f, 0.2f, 1.0f };
	glm::vec4 sunlightDirection{ -0.4f, -0.8f, -0.3f, 3.0f };
	glm::vec4 sunlightColor{ 1.0f, 0.9f, 0.8f, 1.0f };

	// post process settings
	float exposure{ 1.0f };
	float bloomStrength{ 0.04f };
	bool enableSSAO{ true };
};

class SceneSerializer
{
public:
	static bool save_scene(const std::filesystem::path& filePath, entt::registry& registry, const SceneEnvironmentSettings& envSettings);
	static bool load_scene(const std::filesystem::path& filePath, entt::registry& registry, SceneEnvironmentSettings& outEnvSettings, ResourceManager* resourceManager = nullptr, Renderer* renderer = nullptr);

	static std::string serialize_to_string(
		entt::registry& registry,
		const SceneEnvironmentSettings& envSettings);

	static bool deserialize_from_string(const std::string& yamlString, entt::registry& registry, SceneEnvironmentSettings& outEnvSettings, ResourceManager* resourceManager = nullptr, Renderer* renderer = nullptr);

	static bool extract_referenced_assets( const std::filesystem::path& filePath, SceneAssetManifest& outManifest);
};
