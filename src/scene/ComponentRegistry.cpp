#include "ComponentRegistry.h"
#include <core/Components.h>
#include <core/YamlConverters.h>
#include <resources/AssetSerializer.h>
#include <resources/AssetImporter.h>
#include <resources/AssetSerializer.h>
#include <resources/DefaultResources.h>
#include <resources/ResourceManager.h>

namespace
{
	// creates valid, visible fallback PBR MaterialInstance
	std::shared_ptr<MaterialInstance> create_default_pbr_material_instance()
	{
		auto& defaults = DefaultResources::get();
		auto instance = std::make_shared<MaterialInstance>(defaults.defaultPBRTemplate);
		instance->set_alpha_mode(AlphaMode::Opaque);
		instance->set_double_sided(false);
		instance->set_parameter("colorFactors", glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
		instance->set_parameter("metalRoughFactors", glm::vec4(0.0f, 0.8f, 0.0f, 0.0f));
		instance->set_parameter("extraFactors", glm::vec4(1.0f, 1.0f, 0.0f, 0.0f));
		instance->set_parameter("emissiveFactor", glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
		instance->set_texture("albedo", defaults.whiteImage);
		instance->set_texture("normal", defaults.defaultNormalImage);
		instance->set_texture("metalRough", defaults.whiteImage);
		instance->set_texture("occlusion", defaults.whiteImage);
		instance->set_texture("emissive", defaults.blackImage);
		return instance;
	}
}

void ComponentRegistry::init_default_components()
{
	if (_initialized)
		return;

	_initialized = true;

	register_component({
		.name = "TagComponent",
		.hasComponent = [](const entt::registry& r, entt::entity e) { return r.all_of<TagComponent>(e); },
		.addComponent = [](entt::registry& r, entt::entity e) { r.emplace_or_replace<TagComponent>(e, "Entity"); },
		.removeComponent = [](entt::registry& r, entt::entity e) { r.remove<TagComponent>(e); },
		.serialize = [](const entt::registry& r, entt::entity e, YAML::Emitter& out) {
			const auto& tc = r.get<TagComponent>(e);
			out << YAML::Key << "TagComponent" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "tag" << YAML::Value << tc.tag;
			out << YAML::EndMap;
		},
		.deserialize = [](entt::registry& r, entt::entity e, const YAML::Node& node, ResourceManager*) {
			auto& tc = r.emplace_or_replace<TagComponent>(e);
			if (node["tag"])
				tc.tag = node["tag"].as<std::string>();
		}
		});

	// TransformComponent
	register_component({
		.name = "TransformComponent",
		.hasComponent = [](const entt::registry& r, entt::entity e) { return r.all_of<TransformComponent>(e); },
		.addComponent = [](entt::registry& r, entt::entity e) { r.emplace_or_replace<TransformComponent>(e); },
		.removeComponent = [](entt::registry& r, entt::entity e) { r.remove<TransformComponent>(e); },
		.serialize = [](const entt::registry& r, entt::entity e, YAML::Emitter& out) {
			const auto& tc = r.get<TransformComponent>(e);
			out << YAML::Key << "TransformComponent" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "position" << YAML::Value << tc.position;
			out << YAML::Key << "rotation" << YAML::Value << tc.rotation;
			out << YAML::Key << "scale" << YAML::Value << tc.scale;
			out << YAML::EndMap;
		},
		.deserialize = [](entt::registry& r, entt::entity e, const YAML::Node& node, ResourceManager*) {
			auto& tc = r.emplace_or_replace<TransformComponent>(e);
			if (node["position"])
				tc.position = node["position"].as<glm::vec3>();
			if (node["rotation"])
				tc.rotation = node["rotation"].as<glm::vec3>();
			if (node["scale"])
				tc.scale = node["scale"].as<glm::vec3>();
			tc.isDirty = true;
		}
		});

	// MeshComponent
	register_component({
		.name = "MeshComponent",
		.hasComponent = [](const entt::registry& r, entt::entity e) { return r.all_of<MeshComponent>(e); },
		.addComponent = [](entt::registry& r, entt::entity e) { r.emplace_or_replace<MeshComponent>(e); },
		.removeComponent = [](entt::registry& r, entt::entity e) { r.remove<MeshComponent>(e); },
		.serialize = [](const entt::registry& r, entt::entity e, YAML::Emitter& out) {
			const auto& mc = r.get<MeshComponent>(e);
			UUID uuid = mc.meshAssetUUID.is_valid()
					? mc.meshAssetUUID
					: (mc.mesh.is_valid() ? mc.mesh.get_uuid() : UUID::null());
			std::string meshId = mc.mesh.is_valid() ? mc.mesh.get_id() : "";

			out << YAML::Key << "MeshComponent" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "meshAssetUUID" << YAML::Value << uuid;
			out << YAML::Key << "meshId" << YAML::Value << meshId;
			out << YAML::Key << "startIndex" << YAML::Value << mc.startIndex;
			out << YAML::Key << "count" << YAML::Value << mc.count;
			out << YAML::Key << "visible" << YAML::Value << mc.visible;
			out << YAML::Key << "bounds" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "origin" << YAML::Value << mc.bounds.origin;
			out << YAML::Key << "sphereRadius" << YAML::Value << mc.bounds.sphereRadius;
			out << YAML::Key << "extents" << YAML::Value << mc.bounds.extents;
			out << YAML::EndMap;
			out << YAML::EndMap;
		},
		.deserialize = [](entt::registry& r, entt::entity e, const YAML::Node& node, ResourceManager* rm) {
			auto& mc = r.emplace_or_replace<MeshComponent>(e);
			if (node["meshAssetUUID"]) mc.meshAssetUUID = node["meshAssetUUID"].as<UUID>();
			std::string meshId = node["meshId"] ? node["meshId"].as<std::string>("") : "";
			if (node["startIndex"])    mc.startIndex = node["startIndex"].as<uint32_t>();
			if (node["count"])         mc.count = node["count"].as<uint32_t>();
			if (node["visible"])       mc.visible = node["visible"].as<bool>();
			if (node["bounds"])        mc.bounds = node["bounds"].as<Bounds>();

			if (!rm) return;

			// check if mesh is already resident in ResourceManager by UUID or string ID
			if (mc.meshAssetUUID.is_valid() && rm->has_resource<Mesh>(mc.meshAssetUUID)) {
				mc.mesh = rm->get_handle<Mesh>(mc.meshAssetUUID);
			}
			else if (!meshId.empty() && rm->has_resource<Mesh>(meshId)) {
				mc.mesh = rm->get_handle<Mesh>(meshId);
				mc.meshAssetUUID = mc.mesh.get_uuid();
			}
			// otherwise load from cooked .gmesh binary on disk
			else if (mc.meshAssetUUID.is_valid()) {
				std::filesystem::path cookedPath = std::filesystem::path(".cache/meshes") / (mc.meshAssetUUID.to_string() + ".gmesh");
				const auto* meshMeta = AssetRegistry::get().get_metadata(mc.meshAssetUUID);
				if (meshMeta && !meshMeta->cookedPath.empty()) {
					cookedPath = meshMeta->cookedPath;
				}

				// if .gmesh file is missing and belongs to a parent .gltf model, cook parent model 
				if (!std::filesystem::exists(cookedPath) && meshMeta && meshMeta->parentAssetUUID.is_valid()) {
					if (const auto* parentMeta = AssetRegistry::get().get_metadata(meshMeta->parentAssetUUID)) {
						AssetImporter::import_model(*parentMeta);
					}
				}

				if (std::filesystem::exists(cookedPath)) {
					std::string debugName = meshMeta ? meshMeta->name : mc.meshAssetUUID.to_string();
					mc.mesh = rm->load_by_uuid<Mesh>(mc.meshAssetUUID, debugName, cookedPath);
				}
			}

			// ensure valid draw count and bounding volume from loaded mesh
			if (mc.mesh.is_valid()) {
				if (mc.count == 0) {
					mc.count = mc.mesh->get_index_count(0);
				}
				if (mc.bounds.sphereRadius <= 0.0f && mc.mesh->get_bounds().sphereRadius > 0.0f) {
					mc.bounds = mc.mesh->get_bounds();
				}
			}
		}
		});

	// MaterialComponent
	register_component({
		.name = "MaterialComponent",
		.hasComponent = [](const entt::registry& r, entt::entity e) { return r.all_of<MaterialComponent>(e); },
		.addComponent = [](entt::registry& r, entt::entity e) {
			auto& matc = r.emplace_or_replace<MaterialComponent>(e);
			matc.material = create_default_pbr_material_instance();
		},
		.removeComponent = [](entt::registry& r, entt::entity e) { r.remove<MaterialComponent>(e); },
		.serialize = [](const entt::registry& r, entt::entity e, YAML::Emitter& out) {
			const auto& matc = r.get<MaterialComponent>(e);
			out << YAML::Key << "MaterialComponent" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "materialAssetUUID" << YAML::Value << matc.materialAssetUUID;
			out << YAML::Key << "doubleSided" << YAML::Value << matc.doubleSided;
			out << YAML::Key << "alphaMode" << YAML::Value << static_cast<uint32_t>(matc.alphaMode);
			out << YAML::EndMap;
		},
		.deserialize = [](entt::registry& r, entt::entity e, const YAML::Node& node, ResourceManager* rm) {
			auto& matc = r.emplace_or_replace<MaterialComponent>(e);
			if (node["materialAssetUUID"]) matc.materialAssetUUID = node["materialAssetUUID"].as<UUID>();
			if (node["doubleSided"])       matc.doubleSided = node["doubleSided"].as<bool>();
			if (node["alphaMode"])         matc.alphaMode = static_cast<AlphaMode>(node["alphaMode"].as<uint32_t>());
			matc.isDirty = true;

			if (rm && matc.materialAssetUUID.is_valid()) {
				if (const AssetMetadata* meta = AssetRegistry::get().get_metadata(matc.materialAssetUUID)) {
					std::filesystem::path matPath = meta->virtualPath;
					if (matPath.extension() != ".mat") {
						matPath = meta->virtualPath.parent_path() / "materials" / (meta->name + ".mat");
					}

					if (!std::filesystem::exists(matPath) && meta->parentAssetUUID.is_valid()) {
						if (const auto* parentMeta = AssetRegistry::get().get_metadata(meta->parentAssetUUID)) {
							AssetImporter::import_model(*parentMeta);
						}
					}

					MaterialAssetData matData{};
					if (std::filesystem::exists(matPath) && AssetSerializer::load_material_file(matPath, matData)) {
						matc.material = AssetSerializer::build_material_instance(matData, *rm);
						matc.alphaMode = matData.alphaMode;
						matc.doubleSided = matData.doubleSided;
					}
				}
			}

			if (!matc.material && DefaultResources::get().defaultPBRTemplate.is_valid()) {
				matc.material = create_default_pbr_material_instance();
				matc.material->set_alpha_mode(matc.alphaMode);
				matc.material->set_double_sided(matc.doubleSided);
			}
		}
		});

	// PointLightComponent
	register_component({
		.name = "PointLightComponent",
		.hasComponent = [](const entt::registry& r, entt::entity e) { return r.all_of<PointLightComponent>(e); },
		.addComponent = [](entt::registry& r, entt::entity e) { r.emplace_or_replace<PointLightComponent>(e); },
		.removeComponent = [](entt::registry& r, entt::entity e) { r.remove<PointLightComponent>(e); },
		.serialize = [](const entt::registry& r, entt::entity e, YAML::Emitter& out) {
			const auto& pl = r.get<PointLightComponent>(e);
			out << YAML::Key << "PointLightComponent" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "color" << YAML::Value << pl.color;
			out << YAML::Key << "intensity" << YAML::Value << pl.intensity;
			out << YAML::Key << "radius" << YAML::Value << pl.radius;
			out << YAML::Key << "castShadow" << YAML::Value << pl.castShadow;
			out << YAML::Key << "visible" << YAML::Value << pl.visible;
			out << YAML::EndMap;
		},
		.deserialize = [](entt::registry& r, entt::entity e, const YAML::Node& node, ResourceManager*) {
			auto& pl = r.emplace_or_replace<PointLightComponent>(e);
			if (node["color"])
				pl.color = node["color"].as<glm::vec3>();
			if (node["intensity"])
				pl.intensity = node["intensity"].as<float>();
			if (node["radius"])
				pl.radius = node["radius"].as<float>();
			if (node["castShadow"])
				pl.castShadow = node["castShadow"].as<bool>();
			if (node["visible"])
				pl.visible = node["visible"].as<bool>();
		}
		});

	// DirectionalLightComponent
	register_component({
		.name = "DirectionalLightComponent",
		.hasComponent = [](const entt::registry& r, entt::entity e) { return r.all_of<DirectionalLightComponent>(e); },
		.addComponent = [](entt::registry& r, entt::entity e) { r.emplace_or_replace<DirectionalLightComponent>(e); },
		.removeComponent = [](entt::registry& r, entt::entity e) { r.remove<DirectionalLightComponent>(e); },
		.serialize = [](const entt::registry& r, entt::entity e, YAML::Emitter& out) {
			const auto& dl = r.get<DirectionalLightComponent>(e);
			out << YAML::Key << "DirectionalLightComponent" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "color" << YAML::Value << dl.color;
			out << YAML::Key << "intensity" << YAML::Value << dl.intensity;
			out << YAML::Key << "direction" << YAML::Value << dl.direction;
			out << YAML::Key << "shadowExtent" << YAML::Value << dl.shadowExtent;
			out << YAML::Key << "visible" << YAML::Value << dl.visible;
			out << YAML::EndMap;
		},
		.deserialize = [](entt::registry& r, entt::entity e, const YAML::Node& node, ResourceManager*) {
			auto& dl = r.emplace_or_replace<DirectionalLightComponent>(e);
			if (node["color"])
				dl.color = node["color"].as<glm::vec3>();
			if (node["intensity"])
				dl.intensity = node["intensity"].as<float>();
			if (node["direction"])
				dl.direction = node["direction"].as<glm::vec3>();
			if (node["shadowExtent"])
				dl.shadowExtent = node["shadowExtent"].as<float>();
			if (node["visible"])
				dl.visible = node["visible"].as<bool>();
		}
		});
}

void ComponentRegistry::register_component(ComponentRegistryEntry entry)
{
	_entries.push_back(std::move(entry));
}
