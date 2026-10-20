#include "Scene.h"
#include <resources/AssetRegistry.h>
#include <resources/AssetSerializer.h>
#include <algorithm>

void Scene::clear()
{
	_registry.clear();
	_envSettings = SceneEnvironmentSettings{};
}

entt::entity Scene::create_entity(const std::string & tag, const UUID & explicitId)
{
	entt::entity entity = _registry.create();
	UUID id = explicitId.is_valid() ? explicitId : UUID::generate();

	_registry.emplace<IDComponent>(entity, id);
	_registry.emplace<TagComponent>(entity, tag);
	_registry.emplace<TransformComponent>(entity);
	_registry.emplace<HierarchyComponent>(entity);

	return entity;
}

void Scene::destroy_entity(entt::entity entity)
{
	if (entity == entt::null || !_registry.valid(entity))
		return;

	// unlink from parents children if parented
	if (auto* hc = _registry.try_get<HierarchyComponent>(entity)) {
		if (hc->parent != entt::null && _registry.valid(hc->parent)) {
			if (auto* parentHc = _registry.try_get<HierarchyComponent>(hc->parent)) {
				std::erase(parentHc->children, entity);
			}
		}

		// destroy all descendents
		std::vector<entt::entity> childrenCopy = hc->children;
		for (entt::entity child : childrenCopy) {
			destroy_entity(child);
		}
	}

	_registry.destroy(entity);
}

entt::entity Scene::duplicate_entity(entt::entity sourceEntity, entt::entity newParent)
{
	if (sourceEntity == entt::null || !_registry.valid(sourceEntity))
		return entt::null;

	std::string newName = "Entity Copy";
	if (const auto* tag = _registry.try_get<TagComponent>(sourceEntity)) {
		newName = (newParent == entt::null) ? (tag->tag + " (Copy)") : tag->tag;
	}

	entt::entity dst = create_entity(newName);

	if (const auto* tc = _registry.try_get<TransformComponent>(sourceEntity)) {
		auto& dstTc = _registry.get<TransformComponent>(dst);
		dstTc = *tc;
		dstTc.isDirty = true;
	}

	if (const auto* mc = _registry.try_get<MeshComponent>(sourceEntity)) {
		_registry.emplace_or_replace<MeshComponent>(dst, *mc);
	}

	if (const auto* matc = _registry.try_get<MaterialComponent>(sourceEntity)) {
		_registry.emplace_or_replace<MaterialComponent>(dst, *matc);
	}

	if (const auto* pl = _registry.try_get<PointLightComponent>(sourceEntity)) {
		_registry.emplace_or_replace<PointLightComponent>(dst, *pl);
	}

	if (const auto* dl = _registry.try_get<DirectionalLightComponent>(sourceEntity)) {
		_registry.emplace_or_replace<DirectionalLightComponent>(dst, *dl);
	}

	if (const auto* prefabComp = _registry.try_get<PrefabInstanceComponent>(sourceEntity)) {
		_registry.emplace_or_replace<PrefabInstanceComponent>(dst, *prefabComp);
	}

	// preserve parent if duplicating a root selection
	if (newParent == entt::null) {
		if (const auto* srcHc = _registry.try_get<HierarchyComponent>(sourceEntity)) {
			newParent = srcHc->parent;
		}
	}
	if (newParent != entt::null && _registry.valid(newParent)) {
		set_parent(dst, newParent);
	}

	// recursively duplicate children
	if (const auto* srcHc = _registry.try_get<HierarchyComponent>(sourceEntity)) {
		std::vector<entt::entity> childrenCopy = srcHc->children;
		for (entt::entity child : childrenCopy) {
			duplicate_entity(child, dst);
		}
	}

	return dst;
}

void Scene::set_parent(entt::entity child, entt::entity parent)
{
	if (child == entt::null || !_registry.valid(child) || child == parent)
		return;

	auto& childHc = _registry.get_or_emplace<HierarchyComponent>(child);

	// remove from previous parent
	if (childHc.parent != entt::null && _registry.valid(childHc.parent)) {
		if (auto* oldParentHc = _registry.try_get<HierarchyComponent>(childHc.parent)) {
			std::erase(oldParentHc->children, child);
		}
	}

	childHc.parent = parent;

	// add to new parent
	if (parent != entt::null && _registry.valid(parent)) {
		auto& parentHc = _registry.get_or_emplace<HierarchyComponent>(parent);
		if (std::find(parentHc.children.begin(), parentHc.children.end(), child) == parentHc.children.end()) {
			parentHc.children.push_back(child);
		}
	}

	if (auto* tc = _registry.try_get<TransformComponent>(child)) {
		tc->isDirty = true;
	}
}

entt::entity Scene::get_entity_by_uuid(const UUID & uuid) const
{
	if (!uuid.is_valid())
		return entt::null;

	auto view = _registry.view<const IDComponent>();
	for (auto [entity, idComp] : view.each()) {
		if (idComp.id == uuid) {
			return entity;
		}
	}
	return entt::null;
}

SceneAssetManifest Scene::collect_referenced_assets() const
{
	SceneAssetManifest manifest{};
	manifest.hdrEnvironmentUUID = _envSettings.hdrEnvironmentUUID;

	auto meshView = _registry.view<const MeshComponent>();
	for (auto [_, mc] : meshView.each()) {
		UUID id = UUID::null();
		if (mc.meshAssetUUID.is_valid()) {
			id = mc.meshAssetUUID;
		}
		else if (mc.mesh.is_valid()) {
			id = mc.mesh.get_uuid();
		}

		if (id.is_valid()) {
			manifest.meshUUIDs.insert(id);
		}
	}

	auto matView = _registry.view<const MaterialComponent>();
	for (auto [_, matc] : matView.each()) {
		if (!matc.materialAssetUUID.is_valid()) {
			continue;
		}

		manifest.materialUUIDs.insert(matc.materialAssetUUID);

		const auto* meta = AssetRegistry::get().get_metadata(matc.materialAssetUUID);
		if (!meta) {
			continue;
			
		}

		MaterialAssetData matData{};
		if (!AssetSerializer::load_material_file(meta->virtualPath, matData)) {
			continue;
		}

		if (matData.albedoTextureUUID.is_valid())     
			manifest.textureUUIDs.insert(matData.albedoTextureUUID);
		if (matData.normalTextureUUID.is_valid())     
			manifest.textureUUIDs.insert(matData.normalTextureUUID);
		if (matData.metalRoughTextureUUID.is_valid()) 
			manifest.textureUUIDs.insert(matData.metalRoughTextureUUID);
		if (matData.occlusionTextureUUID.is_valid())  
			manifest.textureUUIDs.insert(matData.occlusionTextureUUID);
		if (matData.emissiveTextureUUID.is_valid())   
			manifest.textureUUIDs.insert(matData.emissiveTextureUUID);
	}

	return manifest;
}
