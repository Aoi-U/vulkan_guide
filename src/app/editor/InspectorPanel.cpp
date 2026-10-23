#include "app/editor/InspectorPanel.h"
#include <core/Components.h>
#include <scene/ComponentRegistry.h>
#include <graphics/Renderer.h>
#include <app/editor/UIWidgets.h>
#include <resources/AssetRegistry.h>
#include <resources/AssetImporter.h>
#include <resources/DefaultResources.h>
#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>
#include <fmt/core.h>
#include <fstream>

void InspectorPanel::on_imgui_render(EngineContext& ctx, EditorContext& editorCtx)
{
	if (!ImGui::Begin("Inspector")) {
		ImGui::End();
		return;
	}

	if (editorCtx.inspectorMode == InspectorMode::Entity) {
		draw_entity_inspector(ctx, editorCtx);
	}
	else if (editorCtx.inspectorMode == InspectorMode::DiskAsset) {
		draw_disk_asset_inspector(ctx, editorCtx);
	}
	else {
		ImGui::TextDisabled("Nothing Selected");
	}

	ImGui::End();
}

void InspectorPanel::draw_entity_inspector(EngineContext& ctx, EditorContext& editorCtx)
{
	if (!ctx.activeRegistry) return;
	entt::registry& registry = *ctx.activeRegistry;
	entt::entity entity = editorCtx.selectedEntity;

	if (entity == entt::null || !registry.valid(entity)) {
		editorCtx.clear_selection();
		return;
	}

	// TagComponent and IDComponent
	if (auto* tagComp = registry.try_get<TagComponent>(entity)) {
		char tagBuf[256]{};
		std::snprintf(tagBuf, sizeof(tagBuf), "%s", tagComp->tag.c_str());
		if (ImGui::InputText("Tag", tagBuf, sizeof(tagBuf))) {
			tagComp->tag = tagBuf;
		}
	}

	if (const auto* idComp = registry.try_get<IDComponent>(entity)) {
		ImGui::TextDisabled("Entity UUID: %s", idComp->id.to_string().c_str());
	}

	ImGui::Separator();

	// TransformComponent
	if (auto* tc = registry.try_get<TransformComponent>(entity)) {
		if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
			bool changed = false;
			changed |= UIWidgets::draw_vec3_control("Position", tc->position, glm::vec3(0.0f));
			changed |= UIWidgets::draw_vec3_control("Rotation", tc->rotation, glm::vec3(0.0f), 0.5f);
			changed |= UIWidgets::draw_vec3_control("Scale", tc->scale, glm::vec3(1.0f), 0.05f, &_uniformScaleLocked);
			if (changed) {
				tc->isDirty = true;
				if (ctx.renderer) ctx.renderer->mark_scene_dirty();
			}
		}
	}

	// MeshComponent
	if (auto* mc = registry.try_get<MeshComponent>(entity)) {
		if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
			if (ImGui::Checkbox("Visible", &mc->visible)) {
				if (ctx.renderer) ctx.renderer->mark_scene_dirty();
			}

			UUID meshUUID = mc->meshAssetUUID.is_valid()
				? mc->meshAssetUUID
				: (mc->mesh.is_valid() ? mc->mesh.get_uuid() : UUID::null());

			if (UIWidgets::draw_asset_slot("Mesh Asset (.gmesh)", meshUUID, AssetType::Mesh, true)) {
				mc->meshAssetUUID = meshUUID;
				if (meshUUID.is_valid() && ctx.resourceManager) {
					std::filesystem::path cookedPath = std::filesystem::path(".cache/meshes") / (meshUUID.to_string() + ".gmesh");
					if (const auto* meta = AssetRegistry::get().get_metadata(meshUUID)) {
						if (!meta->cookedPath.empty()) cookedPath = meta->cookedPath;
					}
					auto newMesh = ctx.resourceManager->load_by_uuid<Mesh>(meshUUID, meshUUID.to_string(), cookedPath);
					mc->set_mesh(newMesh);
					if (newMesh.is_valid()) {
						mc->bounds = newMesh->get_bounds();
					}
				}
				else {
					mc->set_mesh({});
				}
				if (ctx.renderer) ctx.renderer->mark_scene_dirty();
			}

			if (mc->mesh.is_valid()) {
				ImGui::TextDisabled("Vertices: %u | Indices (LOD 0): %u | LODs: %u",
					mc->mesh->get_vertex_count(),
					mc->mesh->get_index_count(0),
					mc->mesh->get_lod_count());
			}
		}
	}

	// MaterialComponent 
	if (auto* matc = registry.try_get<MaterialComponent>(entity)) {
		if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
			if (UIWidgets::draw_asset_slot("Material Asset (.mat)", matc->materialAssetUUID, AssetType::Material, true)) {
				if (matc->materialAssetUUID.is_valid() && ctx.resourceManager) {
					if (const auto* meta = AssetRegistry::get().get_metadata(matc->materialAssetUUID)) {
						MaterialAssetData matData{};
						if (AssetSerializer::load_material_file(meta->virtualPath, matData)) {
							matc->material = AssetSerializer::build_material_instance(matData, *ctx.resourceManager);
							matc->alphaMode = matData.alphaMode;
							matc->doubleSided = matData.doubleSided;
							matc->isDirty = true;
						}
					}
				}
				else {
					matc->material = std::make_shared<MaterialInstance>(DefaultResources::get().defaultPBRTemplate);
					matc->isDirty = true;
				}
				if (ctx.renderer) ctx.renderer->mark_scene_dirty();
			}

			if (ImGui::Checkbox("Double Sided", &matc->doubleSided)) {
				if (matc->material) matc->material->set_double_sided(matc->doubleSided);
				matc->isDirty = true;
				if (ctx.renderer) ctx.renderer->mark_scene_dirty();
			}

			const char* alphaModes[] = { "Opaque", "Mask", "Blend" };
			int currentAlpha = static_cast<int>(matc->alphaMode);
			if (ImGui::Combo("Alpha Mode", &currentAlpha, alphaModes, IM_ARRAYSIZE(alphaModes))) {
				matc->alphaMode = static_cast<AlphaMode>(currentAlpha);
				if (matc->material) matc->material->set_alpha_mode(matc->alphaMode);
				matc->isDirty = true;
				if (ctx.renderer) ctx.renderer->mark_scene_dirty();
			}

			if (matc->materialAssetUUID.is_valid()) {
				if (ImGui::Button("Edit .mat Asset in Inspector")) {
					editorCtx.select_disk_asset(matc->materialAssetUUID);
				}
			}
		}
	}

	// PointLightComponent
	if (auto* pl = registry.try_get<PointLightComponent>(entity)) {
		if (ImGui::CollapsingHeader("Point Light", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Checkbox("Visible##PL", &pl->visible);
			ImGui::ColorEdit3("Color##PL", glm::value_ptr(pl->color));
			ImGui::DragFloat("Intensity##PL", &pl->intensity, 1.0f, 0.0f, 10000.0f);
			ImGui::DragFloat("Radius##PL", &pl->radius, 0.2f, 0.1f, 500.0f);
			ImGui::Checkbox("Cast Shadow##PL", &pl->castShadow);
		}
	}

	// DirectionalLightComponent
	if (auto* dl = registry.try_get<DirectionalLightComponent>(entity)) {
		if (ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Checkbox("Visible##DL", &dl->visible);
			ImGui::ColorEdit3("Color##DL", glm::value_ptr(dl->color));
			ImGui::DragFloat("Intensity##DL", &dl->intensity, 0.1f, 0.0f, 100.0f);
			UIWidgets::draw_vec3_control("Direction", dl->direction, glm::vec3(0.4f, 1.0f, 0.3f), 0.02f);
			ImGui::DragFloat("Shadow Extent", &dl->shadowExtent, 0.5f, 5.0f, 250.0f);
		}
	}

	ImGui::Separator();

	// Add Component button
	ComponentRegistry::get().init_default_components();
	if (ImGui::Button("+ Add Component", ImVec2(-1.0f, 0.0f))) {
		ImGui::OpenPopup("AddComponentPopup");
	}

	if (ImGui::BeginPopup("AddComponentPopup")) {
		for (const auto& entry : ComponentRegistry::get().get_entries()) {
			if (!entry.hasComponent(registry, entity)) {
				if (ImGui::MenuItem(entry.name.c_str())) {
					entry.addComponent(registry, entity);
					if (ctx.renderer) ctx.renderer->mark_scene_dirty();
					ImGui::CloseCurrentPopup();
				}
			}
		}
		ImGui::EndPopup();
	}
}

void InspectorPanel::draw_disk_asset_inspector(EngineContext& ctx, EditorContext& editorCtx)
{
	const AssetMetadata* meta = AssetRegistry::get().get_metadata(editorCtx.selectedAssetUUID);
	if (!meta) {
		editorCtx.clear_selection();
		return;
	}

	// reload cached editor structs when selection changes
	if (_loadedDiskAssetUUID != meta->uuid) {
		_loadedDiskAssetUUID = meta->uuid;
		_hasValidMeshHeader = false;

		if (meta->type == AssetType::Material) {
			_editingMaterialData = MaterialAssetData{};
			_editingMaterialData.uuid = meta->uuid;
			_editingMaterialData.name = meta->name;
			AssetSerializer::load_material_file(meta->virtualPath, _editingMaterialData);
		}
		else if (meta->type == AssetType::Texture || meta->type == AssetType::EnvironmentHDR) {
			_editingTextureSettings = meta->textureSettings;
			_hasValidVTexHeader = false;
			std::filesystem::path vtexPath = std::filesystem::path(".cache/textures") / (meta->uuid.to_string() + ".vtex");
			if (std::filesystem::exists(vtexPath))
			{
				std::vector<CookedVTexTileEntry> dummyTable;
				_hasValidVTexHeader = AssetImporter::read_vtex_header(vtexPath, _inspectedVTexHeader, dummyTable);
			}
		}
		else if (meta->type == AssetType::Mesh || meta->type == AssetType::Model) {
			_editingMeshSettings = meta->meshSettings;
			if (!meta->cookedPath.empty() && std::filesystem::exists(meta->cookedPath)) {
				std::ifstream in(meta->cookedPath, std::ios::binary);
				if (in.is_open()) {
					in.read(reinterpret_cast<char*>(&_inspectedMeshHeader), sizeof(CookedMeshHeader));
					_hasValidMeshHeader = (in.good() && _inspectedMeshHeader.magic == GMESH_MAGIC);
				}
			}
		}
	}

	// header Info
	ImGui::Text("Asset: %s", meta->name.c_str());
	ImGui::SameLine();
	UIWidgets::draw_residency_badge(meta->state, meta->gpuSizeBytes);
	ImGui::TextDisabled("Type: %s", AssetUtils::asset_type_to_string(meta->type).data());
	ImGui::TextDisabled("UUID: %s", meta->uuid.to_string().c_str());
	ImGui::TextDisabled("Disk Path: %s", meta->virtualPath.generic_string().c_str());
	if (!meta->cookedPath.empty()) {
		ImGui::TextDisabled("Cooked Path: %s", meta->cookedPath.generic_string().c_str());
	}
	ImGui::Separator();

	if (meta->type == AssetType::Material) {
		draw_material_asset_inspector(*meta, ctx);
	}
	else if (meta->type == AssetType::Texture || meta->type == AssetType::EnvironmentHDR) {
		draw_texture_asset_inspector(*meta, ctx);
	}
	else if (meta->type == AssetType::Mesh || meta->type == AssetType::Model) {
		draw_mesh_asset_inspector(*meta, ctx);
	}
	else {
		ImGui::TextDisabled("Disk size: %.2f KB", static_cast<double>(meta->diskSizeBytes) / 1024.0);
	}
}

void InspectorPanel::draw_material_asset_inspector(const AssetMetadata& meta, EngineContext& ctx)
{
	ImGui::TextUnformatted("PBR Material Properties (.mat)");
	ImGui::Spacing();

	const char* alphaModes[] = { "Opaque", "Mask", "Blend" };
	int currentAlpha = static_cast<int>(_editingMaterialData.alphaMode);
	if (ImGui::Combo("Alpha Mode", &currentAlpha, alphaModes, IM_ARRAYSIZE(alphaModes))) {
		_editingMaterialData.alphaMode = static_cast<AlphaMode>(currentAlpha);
		_editingMaterialData.passType = (_editingMaterialData.alphaMode == AlphaMode::Blend)
			? MaterialPassType::Transparent
			: MaterialPassType::Opaque;
	}

	ImGui::Checkbox("Double Sided", &_editingMaterialData.doubleSided);
	ImGui::ColorEdit4("Base Color Factor", glm::value_ptr(_editingMaterialData.colorFactors));
	ImGui::SliderFloat("Metallic Factor", &_editingMaterialData.metalRoughFactors.x, 0.0f, 1.0f);
	ImGui::SliderFloat("Roughness Factor", &_editingMaterialData.metalRoughFactors.y, 0.04f, 1.0f);
	ImGui::SliderFloat("Occlusion Strength", &_editingMaterialData.occlusionStrength, 0.0f, 1.0f);
	ImGui::SliderFloat("Normal Scale", &_editingMaterialData.normalScale, 0.0f, 3.0f);
	ImGui::SliderFloat("Transmission Factor", &_editingMaterialData.transmissionFactor, 0.0f, 1.0f);
	ImGui::ColorEdit3("Emissive Color", glm::value_ptr(_editingMaterialData.emissiveFactor));
	ImGui::DragFloat("Emissive Strength", &_editingMaterialData.emissiveStrength, 0.1f, 0.0f, 50.0f);

	ImGui::Separator();
	ImGui::TextUnformatted("Texture Slots (UUID 0 = Default Fallback)");
	UIWidgets::draw_asset_slot("Albedo Texture", _editingMaterialData.albedoTextureUUID, AssetType::Texture, true);
	UIWidgets::draw_asset_slot("Normal Texture", _editingMaterialData.normalTextureUUID, AssetType::Texture, true);
	UIWidgets::draw_asset_slot("MetallicRoughness Texture", _editingMaterialData.metalRoughTextureUUID, AssetType::Texture, true);
	UIWidgets::draw_asset_slot("Occlusion Texture", _editingMaterialData.occlusionTextureUUID, AssetType::Texture, true);
	UIWidgets::draw_asset_slot("Emissive Texture", _editingMaterialData.emissiveTextureUUID, AssetType::Texture, true);

	ImGui::Spacing();
	if (ImGui::Button("Save Material (.mat) & Hot-Update Scene", ImVec2(-1.0f, 32.0f))) {
		if (AssetSerializer::save_material_file(meta.virtualPath, _editingMaterialData)) {
			// hot uupdate any active entity in the scene referencing this material UUID
			if (ctx.activeRegistry && ctx.resourceManager) {
				auto newInstance = AssetSerializer::build_material_instance(_editingMaterialData, *ctx.resourceManager);
				auto view = ctx.activeRegistry->view<MaterialComponent>();
				for (auto [entity, matComp] : view.each()) {
					if (matComp.materialAssetUUID == meta.uuid) {
						matComp.material = newInstance;
						matComp.alphaMode = _editingMaterialData.alphaMode;
						matComp.doubleSided = _editingMaterialData.doubleSided;
						matComp.isDirty = true;
					}
				}
				if (ctx.renderer) {
					ctx.renderer->mark_scene_dirty();
				}
			}
		}
	}
}

void InspectorPanel::draw_texture_asset_inspector(const AssetMetadata& meta, EngineContext& ctx)
{
	if (_hasValidVTexHeader) 
	{
		ImGui::TextUnformatted("Cooked .vtex Virtual Texture Telemetry");
		ImGui::BulletText("Virtual Resolution: %u x %u", _inspectedVTexHeader.virtualWidth, _inspectedVTexHeader.virtualHeight);
		ImGui::BulletText("Mip 0 Tile Grid: %u x %u (%u total tiles across %u mips)",
			_inspectedVTexHeader.mips[0].tilesX,
			_inspectedVTexHeader.mips[0].tilesY,
			_inspectedVTexHeader.totalTileCount,
			_inspectedVTexHeader.mipLevels);
		ImGui::BulletText("Tile Footprint: %upx logical + %upx border = %upx physical",
			_inspectedVTexHeader.logicalTileSize,
			_inspectedVTexHeader.borderSize,
			_inspectedVTexHeader.physicalTileSize);
		ImGui::Separator();
	}

	ImGui::TextUnformatted("Texture Import Settings (.meta)");
	ImGui::Spacing();

	ImGui::Checkbox("sRGB Color Space", &_editingTextureSettings.sRGB);
	ImGui::Checkbox("Generate Mipmaps", &_editingTextureSettings.generateMips);
	ImGui::Checkbox("BC7 Compression", &_editingTextureSettings.compress);
	ImGui::Checkbox("Virtual Texture (.vtex)", &_editingTextureSettings.virtualTexture);

	const char* filters[] = { "NEAREST", "LINEAR" };
	int magIdx = (_editingTextureSettings.magFilter == VK_FILTER_NEAREST) ? 0 : 1;
	if (ImGui::Combo("Mag Filter", &magIdx, filters, IM_ARRAYSIZE(filters))) {
		_editingTextureSettings.magFilter = (magIdx == 0) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
	}

	int minIdx = (_editingTextureSettings.minFilter == VK_FILTER_NEAREST) ? 0 : 1;
	if (ImGui::Combo("Min Filter", &minIdx, filters, IM_ARRAYSIZE(filters))) {
		_editingTextureSettings.minFilter = (minIdx == 0) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
	}

	const char* wrapModes[] = { "REPEAT", "MIRRORED_REPEAT", "CLAMP_TO_EDGE", "CLAMP_TO_BORDER" };
	auto wrap_to_idx = [](VkSamplerAddressMode m) -> int {
		switch (m) {
		case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: return 1;
		case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:   return 2;
		case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER: return 3;
		default:                                      return 0;
		}
		};
	auto idx_to_wrap = [](int idx) -> VkSamplerAddressMode {
		switch (idx) {
		case 1:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
		case 2:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		case 3:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
		}
		};

	int wrapU = wrap_to_idx(_editingTextureSettings.addressModeU);
	if (ImGui::Combo("Address Mode U", &wrapU, wrapModes, IM_ARRAYSIZE(wrapModes))) {
		_editingTextureSettings.addressModeU = idx_to_wrap(wrapU);
	}

	int wrapV = wrap_to_idx(_editingTextureSettings.addressModeV);
	if (ImGui::Combo("Address Mode V", &wrapV, wrapModes, IM_ARRAYSIZE(wrapModes))) {
		_editingTextureSettings.addressModeV = idx_to_wrap(wrapV);
	}

	ImGui::SliderFloat("Max Anisotropy", &_editingTextureSettings.anisotropy, 1.0f, 16.0f);

	ImGui::Spacing();
	if (!meta.is_sub_asset() && ImGui::Button("Apply Settings & Recook", ImVec2(-1.0f, 32.0f))) {
		if (AssetMetadata* mutableMeta = AssetRegistry::get().get_metadata_mutable(meta.uuid)) {
			mutableMeta->textureSettings = _editingTextureSettings;
			mutableMeta->cookedPath = std::filesystem::path(".cache/textures") /
				(meta.uuid.to_string() + (_editingTextureSettings.virtualTexture ? ".vtex" : ".gtex"));

			std::filesystem::path metaPath = mutableMeta->virtualPath;
			metaPath += ".meta";
			AssetSerializer::save_meta_file(metaPath, *mutableMeta);
			AssetImporter::cook_texture(*mutableMeta);

			// refresh cached .vtex telemetry in Inspector
			_loadedDiskAssetUUID = UUID::null();
		}
	}
}

void InspectorPanel::draw_mesh_asset_inspector(const AssetMetadata& meta, EngineContext& ctx)
{
	if (_hasValidMeshHeader) {
		ImGui::TextUnformatted("Cooked .gmesh Binary Telemetry");
		ImGui::BulletText("Unique Vertices: %u", _inspectedMeshHeader.vertexCount);
		ImGui::BulletText("Total Indices (All LODs): %u", _inspectedMeshHeader.totalIndexCount);
		ImGui::BulletText("Bounding Sphere Radius: %.3f m", _inspectedMeshHeader.bounds.sphereRadius);
		ImGui::BulletText("Active Discrete LODs: %u", _inspectedMeshHeader.lodCount);

		for (uint32_t i = 0; i < _inspectedMeshHeader.lodCount; ++i) {
			const auto& lod = _inspectedMeshHeader.lods[i];
			ImGui::Text("  LOD %u: %u triangles (%u indices) | Screen Threshold: %.2f",
				i, lod.indexCount / 3, lod.indexCount, lod.screenSizeThreshold);
		}
		ImGui::Separator();
	}

	ImGui::TextUnformatted("Mesh Import & Optimization Settings");
	ImGui::Checkbox("Generate Discrete LODs", &_editingMeshSettings.generateLODs);
	int maxLods = static_cast<int>(_editingMeshSettings.maxLODCount);
	if (ImGui::SliderInt("Max LOD Count", &maxLods, 1, 4)) {
		_editingMeshSettings.maxLODCount = static_cast<uint32_t>(maxLods);
	}
	ImGui::Checkbox("Keep CPU Ray-Picking Data", &_editingMeshSettings.keepCpuPickingData);
	ImGui::Checkbox("Optimize Vertex Cache (ACMR)", &_editingMeshSettings.optimizeVertexCache);
	ImGui::Checkbox("Optimize Overdraw", &_editingMeshSettings.optimizeOverdraw);

	if (meta.type == AssetType::Model && ImGui::Button("Apply & Re-Import Model", ImVec2(-1.0f, 32.0f))) {
		if (AssetMetadata* mutableMeta = AssetRegistry::get().get_metadata_mutable(meta.uuid)) {
			mutableMeta->meshSettings = _editingMeshSettings;
			std::filesystem::path metaPath = mutableMeta->virtualPath;
			metaPath += ".meta";
			AssetSerializer::save_meta_file(metaPath, *mutableMeta);
			AssetImporter::import_model(*mutableMeta);
			if (ctx.renderer) ctx.renderer->mark_scene_dirty();
		}
	}
}
