#pragma once

#include <glm/glm.hpp>
#include <glm/gtx/transform.hpp>
#include <string>

class Model;

struct TagComponent
{
	std::string tag;
};

struct TransformComponent
{
	glm::vec3 position{ 0.0f };
	glm::vec3 rotation{ 0.0f };
	glm::vec3 scale{ 1.0f };

	glm::mat4 get_transform() const
	{
		glm::mat4 rotX = glm::rotate(glm::radians(rotation.x), glm::vec3(1, 0, 0));
		glm::mat4 rotY = glm::rotate(glm::radians(rotation.y), glm::vec3(0, 1, 0));
		glm::mat4 rotZ = glm::rotate(glm::radians(rotation.z), glm::vec3(0, 0, 1));
		glm::mat4 rot = rotZ * rotY * rotX;

		return glm::translate(position) * rot * glm::scale(scale);
	}
};

struct MeshComponent
{
	ResourceHandle<Model> model;
	bool visible{ true };
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