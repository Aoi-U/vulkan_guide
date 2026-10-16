#pragma once

#include <entt.hpp>
#include <resources/ResourceManager.h>
#include <string>
#include <filesystem>
#include <glm/vec3.hpp>

// spawns .prefab hierarchy (cooked .gmesh + .mat assets) directly into ECS
entt::entity spawn_prefab(entt::registry& registry, ResourceManager& resourceManager, const std::filesystem::path& prefabOrModelPath, const glm::vec3& position = glm::vec3(0.0f));
