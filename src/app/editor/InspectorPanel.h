#pragma once

#include <core/layer/EngineContext.h>
#include <app/editor/EditorContext.h>
#include <resources/AssetSerializer.h>
#include <resources/CookedFormats.h>

class InspectorPanel 
{
public:
	void on_imgui_render(EngineContext& ctx, EditorContext& editorCtx);

private:
	void draw_entity_inspector(EngineContext& ctx, EditorContext& editorCtx);
	void draw_disk_asset_inspector(EngineContext& ctx, EditorContext& editorCtx);

	void draw_material_asset_inspector(const AssetMetadata& meta, EngineContext& ctx);
	void draw_texture_asset_inspector(const AssetMetadata& meta, EngineContext& ctx);
	void draw_mesh_asset_inspector(const AssetMetadata& meta, EngineContext& ctx);

	bool _uniformScaleLocked{ true };

	UUID _loadedDiskAssetUUID{};
	MaterialAssetData _editingMaterialData{};
	TextureImportSettings _editingTextureSettings{};
	MeshImportSettings _editingMeshSettings{};
	CookedMeshHeader _inspectedMeshHeader{};
	bool _hasValidMeshHeader{ false };

	CookedVTexHeader _inspectedVTexHeader{};
	bool _hasValidVTexHeader{ false };
};