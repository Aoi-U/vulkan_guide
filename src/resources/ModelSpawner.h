#pragma once

#include <entt.hpp>
#include <resources/ResourceManager.h>
#include <string>
#include <filesystem>
#include <glm/vec3.hpp>

// spawns an entire model hierarchy directly into the ECS
entt::entity spawn_model(entt::registry& registry, ResourceManager& resourceManager, const std::string& modelId, const std::filesystem::path& path, const glm::vec3& position = glm::vec3(0.0f));