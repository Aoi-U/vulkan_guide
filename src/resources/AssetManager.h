#pragma once

#include <core/vk_types.h>
#include <resources/vk_loader.h>
#include <unordered_map>
#include <memory>
#include <string>
#include <filesystem>

class VulkanCore;
class GLTFMetallic_Roughness;

class AssetManager
{
public:
	void init(VulkanCore* vulkanCore, GLTFMetallic_Roughness* materialSystem);
	void cleanup();

	std::shared_ptr<LoadedGLTF> loadScene(const std::string& name, const std::filesystem::path& filePath);

	void unloadScene(const std::string& name);

	std::shared_ptr<LoadedGLTF> getScene(const std::string& name);
	std::shared_ptr<MeshAsset> getMesh(const std::string& key);
	std::unordered_map<std::string, std::shared_ptr<LoadedGLTF>>& getAssets() { return _loadedScenes; }
	std::unordered_map<std::string, std::shared_ptr<MeshAsset>>& getMeshes() { return _loadedMeshes; }

	int numLoadedScenes() const { return static_cast<int>(_loadedScenes.size()); }

private:
	VulkanCore* _vulkanCore{ nullptr };
	GLTFMetallic_Roughness* _materialSystem{ nullptr };
	// cache of loaded gltf files
	std::unordered_map<std::string, std::shared_ptr<LoadedGLTF>> _loadedScenes;

	std::unordered_map<std::string, std::shared_ptr<MeshAsset>> _loadedMeshes;

};