#include "SceneSerializer.h"

#include <scene/Scene.h>
#include <core/Components.h>
#include <scene/ComponentRegistry.h>
#include <core/YamlConverters.h>
#include <core/cvars.h>
#include <resources/AssetRegistry.h>
#include <resources/AssetSerializer.h>
#include <graphics/Renderer.h>
#include <fmt/core.h>
#include <fstream>
#include <unordered_map>

static bool deserialize_yaml(const YAML::Node& root, entt::registry& registry, SceneEnvironmentSettings& outEnvSettings, ResourceManager* resourceManager, Renderer* renderer)
{
	ComponentRegistry::get().init_default_components();

	if (root["sceneUUID"]) outEnvSettings.sceneUUID = root["sceneUUID"].as<UUID>();
	if (root["name"])      outEnvSettings.sceneName = root["name"].as<std::string>();

	if (YAML::Node env = root["environment"]) {
		if (env["skyMode"])            outEnvSettings.useHDRSky = (env["skyMode"].as<std::string>() == "HDR");
		if (env["hdrEnvironmentUUID"]) outEnvSettings.hdrEnvironmentUUID = env["hdrEnvironmentUUID"].as<UUID>();
		if (env["ambientColor"])       outEnvSettings.ambientColor = env["ambientColor"].as<glm::vec4>();
		if (env["sunlightDirection"])  outEnvSettings.sunlightDirection = env["sunlightDirection"].as<glm::vec4>();
		if (env["sunlightColor"])      outEnvSettings.sunlightColor = env["sunlightColor"].as<glm::vec4>();

		if (YAML::Node pp = env["postProcess"]) {
			if (pp["exposure"])      outEnvSettings.exposure = pp["exposure"].as<float>();
			if (pp["bloomStrength"]) outEnvSettings.bloomStrength = pp["bloomStrength"].as<float>();
			if (pp["enableSSAO"])    outEnvSettings.enableSSAO = pp["enableSSAO"].as<bool>();

			CVarSystem::Get()->SetFloatCVar("r.exposure", outEnvSettings.exposure);
			CVarSystem::Get()->SetFloatCVar("r.bloom.strength", outEnvSettings.bloomStrength);
			CVarSystem::Get()->SetIntCVar("r.ssao", outEnvSettings.enableSSAO ? 1 : 0);
		}

		if (renderer) {
			if (outEnvSettings.useHDRSky && outEnvSettings.hdrEnvironmentUUID.is_valid()) {
				if (const auto* hdrMeta = AssetRegistry::get().get_metadata(outEnvSettings.hdrEnvironmentUUID)) {
					renderer->load_environment(hdrMeta->virtualPath);
				}
			}
			else {
				renderer->remove_environment();
			}
		}
	}

	registry.clear();

	YAML::Node entitiesNode = root["entities"];
	if (!entitiesNode || !entitiesNode.IsSequence()) return true;

	struct DeferredHierarchyLinks
	{
		entt::entity entity{ entt::null };
		UUID parentUUID{};
		std::vector<UUID> childrenUUIDs;
	};

	std::unordered_map<UUID, entt::entity> uuidToEntity;
	std::vector<DeferredHierarchyLinks> deferredLinks;
	const auto& componentEntries = ComponentRegistry::get().get_entries();

	// pass 1: create entities and deserialize components
	for (const auto& entityNode : entitiesNode) {
		UUID entityUUID = entityNode["uuid"] ? entityNode["uuid"].as<UUID>() : UUID::generate();
		entt::entity entity = registry.create();
		registry.emplace<IDComponent>(entity, entityUUID);
		uuidToEntity[entityUUID] = entity;

		if (entityNode["prefabAssetUUID"]) {
			registry.emplace<PrefabInstanceComponent>(entity, entityNode["prefabAssetUUID"].as<UUID>());
		}

		if (YAML::Node hcNode = entityNode["HierarchyComponent"]) {
			DeferredHierarchyLinks links{};
			links.entity = entity;
			if (hcNode["parentUUID"]) {
				links.parentUUID = hcNode["parentUUID"].as<UUID>();
			}
			if (YAML::Node cSeq = hcNode["childrenUUIDs"]; cSeq && cSeq.IsSequence()) {
				for (const auto& cUuidNode : cSeq) {
					links.childrenUUIDs.push_back(cUuidNode.as<UUID>());
				}
			}
			deferredLinks.push_back(std::move(links));
		}
		else {
			registry.emplace<HierarchyComponent>(entity);
		}

		for (const auto& entry : componentEntries) {
			if (YAML::Node compNode = entityNode[entry.name]) {
				if (entry.deserialize) {
					entry.deserialize(registry, entity, compNode, resourceManager);
				}
			}
		}
	}

	// pass 2: reconstruct HierarchyComponent links from UUIDs
	for (const auto& link : deferredLinks) {
		auto& hc = registry.emplace_or_replace<HierarchyComponent>(link.entity);
		if (link.parentUUID.is_valid()) {
			if (auto it = uuidToEntity.find(link.parentUUID); it != uuidToEntity.end()) {
				hc.parent = it->second;
			}
		}
		for (const UUID& childUuid : link.childrenUUIDs) {
			if (auto it = uuidToEntity.find(childUuid); it != uuidToEntity.end()) {
				hc.children.push_back(it->second);
			}
		}
	}

	if (renderer) {
		renderer->mark_scene_dirty();
	}
	return true;
}

bool SceneSerializer::save_scene(const std::filesystem::path& filePath, entt::registry& registry, const SceneEnvironmentSettings& envSettings)
{
	if (filePath.has_parent_path()) {
		std::filesystem::create_directories(filePath.parent_path());
	}

	std::string yamlContent = serialize_to_string(registry, envSettings);
	std::ofstream fout(filePath);
	if (!fout.is_open()) {
		fmt::println("SceneSerializer: failed to open {} for writing", filePath.string());
		return false;
	}
	fout << yamlContent;
	return true;
}

bool SceneSerializer::load_scene(const std::filesystem::path& filePath, entt::registry& registry, SceneEnvironmentSettings& outEnvSettings, ResourceManager* resourceManager, Renderer* renderer)
{
	if (!std::filesystem::exists(filePath)) {
		fmt::println("SceneSerializer: scene file does not exist: {}", filePath.string());
		return false;
	}

	try {
		YAML::Node root = YAML::LoadFile(filePath.string());
		return deserialize_yaml(root, registry, outEnvSettings, resourceManager, renderer);
	}
	catch (const YAML::Exception& e) {
		fmt::println("SceneSerializer: YAML error reading scene '{}': {}", filePath.string(), e.what());
		return false;
	}
}

std::string SceneSerializer::serialize_to_string(entt::registry& registry, const SceneEnvironmentSettings& envSettings)
{
	ComponentRegistry::get().init_default_components();

	// ensure all entities have IDComponent prior to serialization
	for (auto entity : registry.storage<entt::entity>()) {
		if (!registry.valid(entity)) 
			continue;
		if (!registry.all_of<IDComponent>(entity)) {
			registry.emplace<IDComponent>(entity, UUID::generate());
		}
	}

	YAML::Emitter out;
	out << YAML::BeginMap;
	out << YAML::Key << "sceneUUID" << YAML::Value << (envSettings.sceneUUID.is_valid() ? envSettings.sceneUUID : UUID::generate());
	out << YAML::Key << "name" << YAML::Value << envSettings.sceneName;
	out << YAML::Key << "version" << YAML::Value << 1;

	// environment & postProcess block
	out << YAML::Key << "environment" << YAML::Value << YAML::BeginMap;
	out << YAML::Key << "skyMode" << YAML::Value << (envSettings.useHDRSky ? "HDR" : "Procedural");
	out << YAML::Key << "hdrEnvironmentUUID" << YAML::Value << envSettings.hdrEnvironmentUUID;
	out << YAML::Key << "ambientColor" << YAML::Value << envSettings.ambientColor;
	out << YAML::Key << "sunlightDirection" << YAML::Value << envSettings.sunlightDirection;
	out << YAML::Key << "sunlightColor" << YAML::Value << envSettings.sunlightColor;

	out << YAML::Key << "postProcess" << YAML::Value << YAML::BeginMap;
	out << YAML::Key << "exposure" << YAML::Value << envSettings.exposure;
	out << YAML::Key << "bloomStrength" << YAML::Value << envSettings.bloomStrength;
	out << YAML::Key << "enableSSAO" << YAML::Value << envSettings.enableSSAO;
	out << YAML::EndMap; // postProcess
	out << YAML::EndMap; // environment

	// entities sequence
	out << YAML::Key << "entities" << YAML::Value << YAML::BeginSeq;

	const auto& componentEntries = ComponentRegistry::get().get_entries();
	auto idView = registry.view<const IDComponent>();

	for (auto [entity, idComp] : idView.each()) {
		out << YAML::BeginMap;
		out << YAML::Key << "uuid" << YAML::Value << idComp.id;

		if (const auto* prefabComp = registry.try_get<PrefabInstanceComponent>(entity)) {
			out << YAML::Key << "prefabAssetUUID" << YAML::Value << prefabComp->prefabAssetUUID;
		}

		// serialize HierarchyComponent via entity UUIDs
		if (const auto* hc = registry.try_get<HierarchyComponent>(entity)) {
			UUID parentUUID = UUID::null();
			if (hc->parent != entt::null && registry.valid(hc->parent) && registry.all_of<IDComponent>(hc->parent)) {
				parentUUID = registry.get<IDComponent>(hc->parent).id;
			}

			std::vector<UUID> childrenUUIDs;
			childrenUUIDs.reserve(hc->children.size());
			for (entt::entity child : hc->children) {
				if (child != entt::null && registry.valid(child) && registry.all_of<IDComponent>(child)) {
					childrenUUIDs.push_back(registry.get<IDComponent>(child).id);
				}
			}

			out << YAML::Key << "HierarchyComponent" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "parentUUID" << YAML::Value << (parentUUID.is_valid() ? parentUUID.to_string() : "0");
			out << YAML::Key << "childrenUUIDs" << YAML::Value << YAML::Flow << childrenUUIDs;
			out << YAML::EndMap;
		}

		// serialize all registered components
		for (const auto& entry : componentEntries) {
			if (entry.hasComponent(registry, entity) && entry.serialize) {
				entry.serialize(registry, entity, out);
			}
		}

		out << YAML::EndMap;
	}

	out << YAML::EndSeq;
	out << YAML::EndMap;

	return std::string(out.c_str());
}

bool SceneSerializer::deserialize_from_string(const std::string& yamlString, entt::registry& registry, SceneEnvironmentSettings& outEnvSettings, ResourceManager* resourceManager, Renderer* renderer)
{
	if (yamlString.empty()) 
		return false;

	try {
		YAML::Node root = YAML::Load(yamlString);
		return deserialize_yaml(root, registry, outEnvSettings, resourceManager, renderer);
	}
	catch (const YAML::Exception& e) {
		fmt::println("SceneSerializer: YAML error deserializing snapshot: {}", e.what());
		return false;
	}if (yamlString.empty()) return false;

	try {
		YAML::Node root = YAML::Load(yamlString);
		return deserialize_yaml(root, registry, outEnvSettings, resourceManager, renderer);
	}
	catch (const YAML::Exception& e) {
		fmt::println("SceneSerializer: YAML error deserializing snapshot: {}", e.what());
		return false;
	}
}

bool SceneSerializer::extract_referenced_assets(const std::filesystem::path& filePath, SceneAssetManifest& outManifest)
{
	if (!std::filesystem::exists(filePath)) 
		return false;

	try {
		YAML::Node root = YAML::LoadFile(filePath.string());

		if (YAML::Node env = root["environment"]) {
			if (env["hdrEnvironmentUUID"]) {
				outManifest.hdrEnvironmentUUID = env["hdrEnvironmentUUID"].as<UUID>();
			}
		}

		YAML::Node entities = root["entities"];
		if (entities && entities.IsSequence()) {
			for (const auto& entityNode : entities) {
				if (YAML::Node mcNode = entityNode["MeshComponent"]) {
					if (mcNode["meshAssetUUID"]) {
						UUID meshId = mcNode["meshAssetUUID"].as<UUID>();
						if (meshId.is_valid()) {
							outManifest.meshUUIDs.insert(meshId);
						}
					}
				}

				if (YAML::Node matNode = entityNode["MaterialComponent"]) {
					if (matNode["materialAssetUUID"]) {
						UUID matId = matNode["materialAssetUUID"].as<UUID>();
						if (matId.is_valid()) {
							outManifest.materialUUIDs.insert(matId);

							if (const auto* matMeta = AssetRegistry::get().get_metadata(matId)) {
								std::filesystem::path matPath = matMeta->virtualPath;
								if (matPath.extension() != ".mat") {
									matPath = matMeta->virtualPath.parent_path() / "materials" / (matMeta->name + ".mat");
								}
								MaterialAssetData matData{};
								if (AssetSerializer::load_material_file(matPath, matData)) {
									if (matData.albedoTextureUUID.is_valid())     outManifest.textureUUIDs.insert(matData.albedoTextureUUID);
									if (matData.normalTextureUUID.is_valid())     outManifest.textureUUIDs.insert(matData.normalTextureUUID);
									if (matData.metalRoughTextureUUID.is_valid()) outManifest.textureUUIDs.insert(matData.metalRoughTextureUUID);
									if (matData.occlusionTextureUUID.is_valid())  outManifest.textureUUIDs.insert(matData.occlusionTextureUUID);
									if (matData.emissiveTextureUUID.is_valid())   outManifest.textureUUIDs.insert(matData.emissiveTextureUUID);
								}
							}
						}
					}
				}
			}
		}
		return true;
	}
	catch (const YAML::Exception&) {
		return false;
	}
}
