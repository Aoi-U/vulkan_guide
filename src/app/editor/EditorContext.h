#pragma once

#include <core/UUID.h>
#include <entt.hpp>
#include <vulkan/vulkan.h>
#include <cstdint>

enum class InspectorMode : uint8_t
{
	None, 
	Entity,
	DiskAsset
};

struct EditorContext
{
	InspectorMode inspectorMode{ InspectorMode::None };
	entt::entity selectedEntity;
	UUID selectedAssetUUID{};

	bool viewportHovered{ false };
	bool viewportFocused{ false };
	VkExtent2D viewportExtent{ 1280, 720 };

	int gizmoOperation{ 7 }; // ImGuizmo state (7 = translate, 120 = rotate, 896 = scale; 0 = local, 1 = world)
	int gizmoMode{ 1 };
	bool useSnap{ false };
	float snapTranslation{ 0.5f };
	float snapRotation{ 15.0f };
	float snapScale{ 0.1f };

	std::string selectedDebugTarget{ "FinalColor" };
	int selectedTargetLayer{ 0 };

	void select_entity(entt::entity e) noexcept
	{
		selectedEntity = e;
		selectedAssetUUID = UUID::null();
		inspectorMode = (e != entt::null) ? InspectorMode::Entity : InspectorMode::None;
	}

	void select_disk_asset(const UUID& uuid) noexcept
	{
		selectedAssetUUID = uuid;
		selectedEntity = entt::null;
		inspectorMode = uuid.is_valid() ? InspectorMode::DiskAsset : InspectorMode::None;
	}

	void clear_selection() noexcept
	{
		selectedEntity = entt::null;
		selectedAssetUUID = UUID::null();
		inspectorMode = InspectorMode::None;
	}
};