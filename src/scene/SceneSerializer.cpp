#include "SceneSerializer.h"

#include <core/Components.h>
#include <core/ComponentRegistry.h>
#include <core/YamlConverters.h>
#include <core/cvars.h>
#include <resources/AssetRegistry.h>
#include <graphics/Renderer.h>
#include <fmt/core.h>
#include <fstream>
#include <unordered_map>

bool SceneSerializer::save_scene(const std::filesystem::path& filePath, entt::registry& registry, const SceneEnvironmentSettings& envSettings)
{
	ComponentRegistry::get().init_default_components();

	if (filePath.has_parent_path()) {
		std::filesystem::create_directories(filePath.parent_path());
	}

	// ensure all entities have an IDComponent
	for (auto entity : registry.storage<entt::entity>()) {
		if (!registry.valid(entity))
			continue;

		if (!registry.all_of<IDComponent>(entity)) {
			registry.emplace<IDComponent>(entity, UUID::generate());
		}
	}

	// serialize scene to YAML
	YAML::Emitter out;

	out << YAML::BeginMap;
	out << YAML::Key << "sceneUUID" << YAML::Value << (envSettings.sceneUUID.is_valid() ? envSettings.sceneUUID : UUID::generate());
	out << YAML::Key << "name" << YAML::Value << envSettings.sceneName;
	out << YAML::Key << "version" << YAML::Value << 1;

	// environment and postprocess block
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

		// serialize HierarchyComponent by entity UUIDs
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

	// write to file
	std::ofstream fout(filePath);
	if (!fout.is_open()) {
		fmt::println("SceneSerializer: failed to open {} for writing", filePath.string());
		return false;
	}

	fout << out.c_str();
	return true;
}

bool SceneSerializer::load_scene(const std::filesystem::path& filePath, entt::registry& registry, SceneEnvironmentSettings& outEnvSettings, ResourceManager* resourceManager, Renderer* renderer)
{
	if (!std::filesystem::exists(filePath)) {
		fmt::println("SceneSerializer: scene file does not exist: {}", filePath.string());
		return false;
	}

	ComponentRegistry::get().init_default_components();

	// load scene from YAML
	try {
		YAML::Node root = YAML::LoadFile(filePath.string());
		if (root["sceneUUID"]) 
			outEnvSettings.sceneUUID = root["sceneUUID"].as<UUID>();
		if (root["name"])      
			outEnvSettings.sceneName = root["name"].as<std::string>();

		if (YAML::Node env = root["environment"]) {
			if (env["skyMode"])            
				outEnvSettings.useHDRSky = (env["skyMode"].as<std::string>() == "HDR");
			if (env["hdrEnvironmentUUID"]) 
				outEnvSettings.hdrEnvironmentUUID = env["hdrEnvironmentUUID"].as<UUID>();
			if (env["ambientColor"])       
				outEnvSettings.ambientColor = env["ambientColor"].as<glm::vec4>();
			if (env["sunlightDirection"])  
				outEnvSettings.sunlightDirection = env["sunlightDirection"].as<glm::vec4>();
			if (env["sunlightColor"])      
				outEnvSettings.sunlightColor = env["sunlightColor"].as<glm::vec4>();

			if (YAML::Node pp = env["postProcess"]) {
				if (pp["exposure"])      
					outEnvSettings.exposure = pp["exposure"].as<float>();
				if (pp["bloomStrength"]) 
					outEnvSettings.bloomStrength = pp["bloomStrength"].as<float>();
				if (pp["enableSSAO"])    
					outEnvSettings.enableSSAO = pp["enableSSAO"].as<bool>();

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
		if (!entitiesNode || !entitiesNode.IsSequence()) 
			return true;

		struct DeferredHierarchyLinks
		{
			entt::entity entity{ entt::null };
			UUID parentUUID{};
			std::vector<UUID> childrenUUIDs;
		};

		std::unordered_map<UUID, entt::entity> uuidToEntity;
		std::vector<DeferredHierarchyLinks> deferredLinks;
		const auto& componentEntries = ComponentRegistry::get().get_entries();

		// create entities and deserialize components
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

			for (const auto& entry : componentEntries) {
				if (YAML::Node compNode = entityNode[entry.name]) {
					if (entry.deserialize) {
						entry.deserialize(registry, entity, compNode, resourceManager);
					}
				}
			}
		}

		// reconstruct HierarchyComponent links from UUIDs
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
	catch (const YAML::Exception& e) {
		fmt::println("SceneSerializer: YAML error reading scene '{}': {}", filePath.string(), e.what());
		return false;
	}
}
