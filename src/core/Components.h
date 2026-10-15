#pragma once

#include <glm/glm.hpp>
#include <glm/gtx/transform.hpp>
#include <core/UUID.h>
#include <resources/Mesh.h>
#include <resources/Texture.h>
#include <resources/MaterialInstance.h>
#include <string>

struct IDComponent
{
	UUID id{ UUID::generate() };
};

struct TagComponent
{
	std::string tag;
};

struct TransformComponent
{
	glm::vec3 position{ 0.0f };
	glm::vec3 rotation{ 0.0f };
	glm::vec3 scale{ 1.0f };
	glm::mat4 worldTransform{ 1.0f };

	bool isDirty{ true };

	glm::mat4 get_transform() const
	{
		glm::mat4 rotX = glm::rotate(glm::radians(rotation.x), glm::vec3(1, 0, 0));
		glm::mat4 rotY = glm::rotate(glm::radians(rotation.y), glm::vec3(0, 1, 0));
		glm::mat4 rotZ = glm::rotate(glm::radians(rotation.z), glm::vec3(0, 0, 1));
		glm::mat4 rot = rotZ * rotY * rotX;

		return glm::translate(position) * rot * glm::scale(scale);
	}
};

struct HierarchyComponent
{
	entt::entity parent{ entt::null };
	std::vector<entt::entity> children;

	HierarchyComponent() = default;
	HierarchyComponent(entt::entity p)
		: parent(p) {}
};

struct MeshComponent
{
	ResourceHandle<Mesh> mesh;
	Bounds bounds{};
	uint32_t startIndex{ 0 };
	uint32_t count{ 0 };
	bool visible{ true };

	void set_mesh(ResourceHandle<Mesh> newMesh)
	{
		mesh = std::move(newMesh);
		startIndex = 0;
		if (mesh.is_valid()) {
			count = mesh->get_index_count();
		}
		else {
			count = 0;
			bounds = {};
		}
	}
};

struct MaterialComponent {
	std::shared_ptr<MaterialInstance> material;
	bool doubleSided{ false };
	AlphaMode alphaMode{ AlphaMode::Opaque };

	// flag to mark GPU descriptor/uniform buffer as dirty when parameters change
	mutable bool isDirty{ true };

};

struct PointLightComponent
{
	glm::vec3 color{ 1.0f, 1.0f, 1.0f };
	float intensity{ 300.0f };
	float radius{ 10.0f };
	bool castShadow{ false };
	bool visible{ true };
};

struct DirectionalLightComponent
{
	glm::vec3 color{ 1.0f, 1.0f, 1.0f };
	float intensity{ 3.0f };
	glm::vec3 direction{ 0.4f, 1.0f, 0.3f };
	float shadowExtent{ 35.0f };
	bool visible{ true };
};