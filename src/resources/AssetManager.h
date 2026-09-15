#pragma once

#include <core/vk_types.h>
#include <vk_loader.h>
#include <unordered_map>
#include <memory>
#include <string>
#include <filesystem>

class VulkanEngine;

struct ModelInstance
{
	std::shared_ptr<LoadedGLTF> asset{ nullptr };
	glm::vec3 position{ 0.0f };
	glm::vec3 rotation{ 0.0f };
	glm::vec3 scale{ 1.0f };

	bool lockScale{ true };

	void draw(const glm::mat4& topMatrix, DrawContext& ctx) const;
};

class AssetManager
{
public:
	AssetManager() = default;
	~AssetManager() = default;

	void init(VulkanEngine* engine);
	void cleanup();

	std::shared_ptr<LoadedGLTF> loadScene(const std::string& name, const std::filesystem::path& filePath);

	void unloadScene(const std::string& name);

	ModelInstance& createModelInstance(const std::string& instanceName, const std::string& assetName);
	void destroyInstance(const std::string& instanceName);


	std::shared_ptr<LoadedGLTF> getScene(const std::string& name);
	std::shared_ptr<MeshAsset> getMesh(const std::string& key);
	std::unordered_map<std::string, ModelInstance>& getInstances() { return _instances; }
	std::unordered_map<std::string, std::shared_ptr<LoadedGLTF>>& getAssets() { return _loadedScenes; }
	std::unordered_map<std::string, std::shared_ptr<MeshAsset>>& getMeshes() { return _loadedMeshes; }

	int numLoadedScenes() const { return static_cast<int>(_loadedScenes.size()); }
	int numLoadedInstances() const { return static_cast<int>(_instances.size()); }

private:
	VulkanEngine* _engine{ nullptr };

	// cache of loaded gltf files
	std::unordered_map<std::string, std::shared_ptr<LoadedGLTF>> _loadedScenes;

	std::unordered_map<std::string, std::shared_ptr<MeshAsset>> _loadedMeshes;

	// active world model instances
	std::unordered_map<std::string, ModelInstance> _instances;
};