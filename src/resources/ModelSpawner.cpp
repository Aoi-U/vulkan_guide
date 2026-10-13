#include "resources/ModelSpawner.h"

#include <core/Components.h>
#include <resources/Model.h>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/quaternion.hpp>

static entt::entity spawn_node(entt::registry& registry, const std::shared_ptr<ModelNode>& node, entt::entity parentEntity) {
	entt::entity nodeEntity = registry.create();
	registry.emplace<TagComponent>(nodeEntity, "Node");

	glm::vec3 scale;
	glm::quat rotation;
	glm::vec3 translation;
	glm::vec3 skew;
	glm::vec4 perspective;
	glm::decompose(node->localTransform, scale, rotation, translation, skew, perspective);

	auto& tc = registry.emplace<TransformComponent>(nodeEntity);
	tc.position = translation;
	tc.rotation = glm::degrees(glm::eulerAngles(rotation));
	tc.scale = scale;
	tc.isDirty = true;

	auto& hc = registry.emplace<HierarchyComponent>(nodeEntity, parentEntity);
	if (parentEntity != entt::null) {
		if (auto* parentHc = registry.try_get<HierarchyComponent>(parentEntity)) {
			parentHc->children.push_back(nodeEntity);
		}
	}

	if (!node->submeshes.empty()) {
		if (node->submeshes.size() == 1) {
			const auto& sm = node->submeshes[0];
			registry.emplace<MeshComponent>(nodeEntity, MeshComponent{
					.mesh = sm.mesh,
					.bounds = sm.bounds,
					.startIndex = sm.startIndex,
					.count = sm.count,
					.visible = true
				});
			if (sm.material) {
				MaterialComponent mat{};
				mat.material = sm.material;
				mat.isDirty = true;
				registry.emplace<MaterialComponent>(nodeEntity, mat);
			}
		}
		else {
			for (size_t i = 0; i < node->submeshes.size(); ++i) {
				const auto& sm = node->submeshes[i];
				entt::entity smEntity = registry.create();
				registry.emplace<TagComponent>(smEntity, "Submesh " + std::to_string(i));

				auto& smTc = registry.emplace<TransformComponent>(smEntity);
				smTc.isDirty = true;

				auto& smHc = registry.emplace<HierarchyComponent>(smEntity, nodeEntity);
				hc.children.push_back(smEntity);

				registry.emplace<MeshComponent>(smEntity, MeshComponent{
					.mesh = sm.mesh,
					.bounds = sm.bounds,
					.startIndex = sm.startIndex,
					.count = sm.count,
					.visible = true
					});
				if (sm.material) {
					MaterialComponent mat{};
					mat.material = sm.material;
					mat.isDirty = true;
					registry.emplace<MaterialComponent>(smEntity, mat);
				}
			}
		}
	}

	for (const auto& childNode : node->children) {
		if (auto childModelNode = std::dynamic_pointer_cast<ModelNode>(childNode)) {
			spawn_node(registry, childModelNode, nodeEntity);
		}
	}

	return nodeEntity;
}

entt::entity spawn_model(entt::registry& registry, ResourceManager& resourceManager, const std::string& modelId, const std::filesystem::path& path, const glm::vec3& position) {
	auto modelHandle = resourceManager.load<Model>(modelId, path);
	if (!modelHandle.is_valid())
		return entt::null;
	
	// wait for the model to be fully loaded before spawning
	modelHandle->wait_until_ready();

	//entt::entity firstRootEntity = registry.create();
	entt::entity firstRootEntity = entt::null;
	for (const auto& rootNode : modelHandle->rootNodes) {
		if (auto childModelNode = std::dynamic_pointer_cast<ModelNode>(rootNode)) {
			entt::entity rootNodeEntity = spawn_node(registry, childModelNode, entt::null);

			// apply spawn position to the roots
			if (auto* tc = registry.try_get<TransformComponent>(rootNodeEntity)) {
				tc->position += position;
				tc->isDirty = true;
			}
			// use the model name to identify the roots in hierarchy
			if (auto* tag = registry.try_get<TagComponent>(rootNodeEntity)) {
				tag->tag = modelId;
			}
			
			if (firstRootEntity == entt::null) {
				firstRootEntity = rootNodeEntity;
			}
		}
	}

	return firstRootEntity;
}