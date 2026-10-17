#include "resources/ModelSpawner.h"
#include <resources/AssetImporter.h>
#include <resources/AssetSerializer.h>
#include <resources/AssetRegistry.h>
#include <resources/DefaultResources.h>
#include <core/Components.h>


entt::entity spawn_prefab(entt::registry& registry, ResourceManager& resourceManager, const std::filesystem::path& prefabOrModelPath, const glm::vec3& position)
{
	std::filesystem::path prefabPath = prefabOrModelPath;
	const AssetMetadata* sourceModelMeta = nullptr;

	if (prefabPath.extension() == ".gltf" || prefabPath.extension() == ".glb") {
		sourceModelMeta = AssetRegistry::get().find_asset_by_path(prefabPath);
		if (sourceModelMeta) {
			std::filesystem::path candidate = prefabPath;
			candidate.replace_extension(".prefab");
			if (!std::filesystem::exists(candidate) || sourceModelMeta->subAssets.empty()) {
				AssetImporter::import_model(*sourceModelMeta);
			}
			prefabPath = candidate;
		}
	}

	PrefabAssetData prefab{};
	if (!AssetSerializer::load_prefab_file(prefabPath, prefab) || prefab.nodes.empty()) {
		return entt::null;
	}

	if (!sourceModelMeta && prefab.sourceModelUUID.is_valid()) {
		sourceModelMeta = AssetRegistry::get().get_metadata(prefab.sourceModelUUID);
	}

	if (sourceModelMeta) {
		bool cacheMissing = sourceModelMeta->subAssets.empty();

		// check if any subasset texture (.gtex) or mesh (.gmesh) is missing on disk
		for (const auto& sub : sourceModelMeta->subAssets) {
			if (sub.type == AssetType::Texture) {
				std::filesystem::path gtex = std::filesystem::path(".cache/textures") / (sub.uuid.to_string() + ".gtex");
				if (!std::filesystem::exists(gtex)) {
					cacheMissing = true;
					break;
				}
			}
			else if (sub.type == AssetType::Mesh) {
				std::filesystem::path gmesh = std::filesystem::path(".cache/meshes") / (sub.uuid.to_string() + ".gmesh");
				if (!std::filesystem::exists(gmesh)) {
					cacheMissing = true;
					break;
				}
			}
		}

		auto check_tex_uuid = [](UUID texUUID) -> bool {
			if (!texUUID.is_valid())
				return true;
			std::filesystem::path gtex = std::filesystem::path(".cache/textures") / (texUUID.to_string() + ".gtex");
			return std::filesystem::exists(gtex);
		};

		// verify prefab submesh bindings and referenced materials/textures
		for (const auto& node : prefab.nodes) {
			for (const auto& sm : node.submeshes) {
				if (sm.meshAssetUUID.is_valid()) {
					std::filesystem::path gmesh = std::filesystem::path(".cache/meshes") / (sm.meshAssetUUID.to_string() + ".gmesh");
					if (!std::filesystem::exists(gmesh)) {
						cacheMissing = true;
						break;
					}
				}
				if (sm.materialAssetUUID.is_valid()) {
					const auto* matMeta = AssetRegistry::get().get_metadata(sm.materialAssetUUID);
					if (!matMeta) {
						cacheMissing = true;
						break;
					}
					std::filesystem::path matPath = matMeta->virtualPath;
					if (matPath.extension() != ".mat") {
						matPath = prefabPath.parent_path() / "materials" / (matMeta->name + ".mat");
					}
					MaterialAssetData matData{};
					if (!AssetSerializer::load_material_file(matPath, matData) ||
						matData.uuid != sm.materialAssetUUID ||
						!check_tex_uuid(matData.albedoTextureUUID) ||
						!check_tex_uuid(matData.metalRoughTextureUUID) ||
						!check_tex_uuid(matData.normalTextureUUID) ||
						!check_tex_uuid(matData.occlusionTextureUUID) ||
						!check_tex_uuid(matData.emissiveTextureUUID)) {
						cacheMissing = true;
						break;
					}
				}
			}
			if (cacheMissing) 
				break;
		}

		if (cacheMissing) {
			AssetImporter::import_model(*sourceModelMeta);
			AssetSerializer::load_prefab_file(prefabPath, prefab);
		}
	}

	std::vector<entt::entity> nodeEntities(prefab.nodes.size(), entt::null);
	entt::entity firstRootEntity = entt::null;

	// create entities and transforms for each node in the prefab
	for (size_t i = 0; i < prefab.nodes.size(); i++) {
		const PrefabNodeData node = prefab.nodes[i];
		entt::entity e = registry.create();
		nodeEntities[i] = e;

		registry.emplace<IDComponent>(e, UUID::generate());
		registry.emplace<TagComponent>(e, node.name);

		auto& tc = registry.emplace<TransformComponent>(e);
		tc.position = node.position;
		tc.rotation = node.rotation;
		tc.scale = node.scale;
		tc.isDirty = true;

		registry.emplace<HierarchyComponent>(e);

		if (node.parentIndex < 0) {
			tc.position += position;
			registry.emplace<PrefabInstanceComponent>(e, prefab.uuid);
			if (firstRootEntity == entt::null) {
				firstRootEntity = e;
			}
		}
	}

	std::unordered_map<UUID, MaterialComponent> resolvedMaterials;

	// create parent-child relationships and attach mesh and material components
	auto resolve_submesh_components = [&](entt::entity targetEntity, const PrefabSubmeshBinding& sm) {
		ResourceHandle<Mesh> meshHandle;
		if (sm.meshAssetUUID.is_valid()) {
			std::filesystem::path cookedPath = std::filesystem::path(".cache/meshes") / (sm.meshAssetUUID.to_string() + ".gmesh");
			if (const auto* meshMeta = AssetRegistry::get().get_metadata(sm.meshAssetUUID)) {
				if (!meshMeta->cookedPath.empty())
					cookedPath = meshMeta->cookedPath;
			}
			meshHandle = resourceManager.load_by_uuid<Mesh>(sm.meshAssetUUID, sm.meshAssetUUID.to_string(), cookedPath);
		}

		registry.emplace<MeshComponent>(targetEntity, MeshComponent{
			.meshAssetUUID = sm.meshAssetUUID,
			.mesh = meshHandle,
			.bounds = meshHandle.is_valid() ? meshHandle->get_bounds() : sm.bounds,
			.startIndex = 0,
			.count = meshHandle.is_valid() ? meshHandle->get_index_count(0) : sm.indexCount,
			.visible = true
			});

		MaterialComponent matComp{};
		matComp.materialAssetUUID = sm.materialAssetUUID;
		if (sm.materialAssetUUID.is_valid()) {
			auto cachedIt = resolvedMaterials.find(sm.materialAssetUUID);
			if (cachedIt != resolvedMaterials.end()) {
				matComp = cachedIt->second;
			}
			else if (const auto* matMeta = AssetRegistry::get().get_metadata(sm.materialAssetUUID)) {
				std::filesystem::path matPath = matMeta->virtualPath;
				if (matPath.extension() != ".mat") {
					matPath = prefabPath.parent_path() / "materials" / (matMeta->name + ".mat");
				}
				MaterialAssetData matData{};
				if (AssetSerializer::load_material_file(matPath, matData)) {
					matComp.material = AssetSerializer::build_material_instance(matData, resourceManager);
					matComp.alphaMode = matData.alphaMode;
					matComp.doubleSided = matData.doubleSided;
					resolvedMaterials[sm.materialAssetUUID] = matComp;
				}
			}
		}

		if (!matComp.material) {
			matComp.material = std::make_shared<MaterialInstance>(DefaultResources::get().defaultPBRTemplate);
		}
		matComp.isDirty = true;
		registry.emplace<MaterialComponent>(targetEntity, std::move(matComp));
	};

	for (size_t i = 0; i < prefab.nodes.size(); i++) {
		const PrefabNodeData& node = prefab.nodes[i];
		entt::entity nodeEntity = nodeEntities[i];
		auto& hc = registry.get<HierarchyComponent>(nodeEntity);

		if (node.parentIndex >= 0 && static_cast<size_t>(node.parentIndex) < nodeEntities.size()) {
			hc.parent = nodeEntities[node.parentIndex];
		}

		for (uint32_t childIdx : node.childrenIndices) {
			if (childIdx < nodeEntities.size()) {
				hc.children.push_back(nodeEntities[childIdx]);
			}
		}

		if (node.submeshes.size() == 1) {
			resolve_submesh_components(nodeEntity, node.submeshes[0]);
		}
		else if (node.submeshes.size() > 1) {
			for (size_t s = 0; s < node.submeshes.size(); s++) {
				entt::entity smEntity = registry.create();
				registry.emplace<IDComponent>(smEntity, UUID::generate());
				registry.emplace<TagComponent>(smEntity, fmt::format("{} submesh {}", node.name, s));

				auto& smTc = registry.emplace<TransformComponent>(smEntity);
				smTc.isDirty = true;
				registry.emplace<HierarchyComponent>(smEntity, nodeEntity);
				hc.children.push_back(smEntity);

				resolve_submesh_components(smEntity, node.submeshes[s]);
			}
		}
	}

	return firstRootEntity;
}
