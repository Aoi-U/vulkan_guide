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