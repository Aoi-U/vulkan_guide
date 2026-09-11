#include "AssetManager.h"

#include <engine/vk_engine.h>
#include <glm/gtx/transform.hpp>
#include <iostream>

void ModelInstance::draw(const glm::mat4& topMatrix, DrawContext& ctx) const
{
	if (!asset) {
		fmt::println("ModelInstance::draw asset is null");
		return;
	}

	glm::mat4 transform{ 1.0f };
	transform = glm::translate(transform, position);
	transform = glm::rotate(transform, glm::radians(rotation.x), glm::vec3{ 1.f, 0.f, 0.f });
	transform = glm::rotate(transform, glm::radians(rotation.y), glm::vec3{ 0.f, 1.f, 0.f });
	transform = glm::rotate(transform, glm::radians(rotation.z), glm::vec3{ 0.f, 0.f, 1.f });
	transform = glm::scale(transform, scale);

	glm::mat4 modelMatrix = topMatrix * transform;

	asset->draw(modelMatrix, ctx);
}

void AssetManager::init(VulkanEngine* engine)
{
	_engine = engine;
}

void AssetManager::cleanup()
{
	_instances.clear();
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
	auto s = loadGltf(_engine, filePath.string());
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
		for (auto instance = _instances.begin(); instance != _instances.end(); instance++) {
			if (instance->second.asset == it->second) {
				instance = _instances.erase(instance);
			}
			else {
				instance++;
			}
		}

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

ModelInstance& AssetManager::createModelInstance(const std::string& instanceName, const std::string& assetName)
{
	auto scene = getScene(assetName);
	_instances[instanceName] = ModelInstance{ .asset = scene };
	return _instances[instanceName];
}

void AssetManager::destroyInstance(const std::string& instanceName)
{
	_instances.erase(instanceName);
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
