#pragma once

#include <entt.hpp>
#include <imgui.h>
#include <resources/ResourceManager.h>
#include <graphics/pass/Pass.h>
#include <graphics/utils/vk_types.h>

class Renderer; 

struct EditorContext
{
	entt::registry& registry;
	ResourceManager& resourceManager;
	entt::entity& selectedEntity;
	const GPUSceneData& sceneData;
	Renderer& renderer;

	ImVec2 viewportPos{ 0.0f, 0.0f };
	ImVec2 viewportSize{ 0.0f, 0.0f };
	bool isViewportHovered{ false };
	bool isViewportFocused{ false };
	bool isGizmoControlsHovered{ false };
};