#pragma once

#include <entt.hpp>
#include <resources/AssetManager.h>
#include <graphics/pass/Pass.h>

struct EngineStats
{
	float frameTimeMs{ 0.0f };
	float fps{ 0.0f };
	std::vector<PassStats> renderStats;
};

struct EditorContext
{
	entt::registry& registry;
	AssetManager& assetManager;
	const EngineStats& stats;
	entt::entity& selectedEntity;
};