#include "AssetManager.h"

#include <graphics/VulkanCore.h>
#include <glm/gtx/transform.hpp>
#include <iostream>

void AssetManager::init(VulkanCore* vulkanCore, GLTFMetallic_Roughness* materialSystem)
{
	_vulkanCore = vulkanCore;
	_materialSystem = materialSystem;
}

void AssetManager::cleanup()
{
	_loadedMeshes.clear();

	_loadedScenes.clear();
}

std::shared_ptr<LoadedGLTF> AssetManager::loadScene(const std::string& name, const std::filesystem::path& filePath)
{
	// check if the gltf is already loaded
	auto it = _loadedScenes.find(name);
	if (it != _loadedScenes.end()) {
		return it->second;
	}

	// load the gltf file
	auto s = loadGltf(_vulkanCore, *_materialSystem, filePath.string());
	if (!s.has_value()) {
		std::cerr << "AssetManager: failed to load scene from: " << filePath << std::endl;
		return nullptr;
	}

	std::shared_ptr<LoadedGLTF> scene = s.value();
	_loadedScenes[name] = scene;

	for (auto& [meshName, mesh] : scene->meshes) {
		std::string key = name + "::" + meshName;
		_loadedMeshes[key] = mesh;
	}

	return scene;
}

void AssetManager::unloadScene(const std::string& name)
{
	auto it = _loadedScenes.find(name);
	if (it != _loadedScenes.end()) {
		for (auto mesh = _loadedMeshes.begin(); mesh != _loadedMeshes.end(); mesh++) {
			if (mesh->first.rfind(name + "::", 0) == 0) {
				mesh = _loadedMeshes.erase(mesh);
			}
			else {
				mesh++;
			}
		}

		// destroy the loaded scene
		_loadedScenes.erase(it);
	}
}

std::shared_ptr<LoadedGLTF> AssetManager::getScene(const std::string& name)
{
	auto it = _loadedScenes.find(name);
	return (it != _loadedScenes.end()) ? it->second : nullptr;
}

std::shared_ptr<MeshAsset> AssetManager::getMesh(const std::string& key)
{
	auto it = _loadedMeshes.find(key);
	return (it != _loadedMeshes.end()) ? it->second : nullptr;
}
