// vulkan_guide.h : Include file for standard system include files,
// or project specific include files.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <span>
#include <array>
#include <functional>
#include <deque>

#include <vulkan/vulkan.h>
#include <vulkan/vk_enum_string_helper.h>
#include <vk_mem_alloc.h>

#include <fmt/core.h>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

// NOTE: inefficient at scale. better implementation would be to store arrays of vulkan handles of various types (VkImage, VkBuffer, etc)
// and delete them from a loop
struct DeletionQueue
{
	std::deque<std::function<void()>> deletors;

	// adds a function to the deletion queue
	void push_function(std::function<void()>&& function)
	{
		deletors.push_back(function);
	}

	// flushes the deletion queue
	void flush()
	{
		// reverse iterate the deletion queue to execute all functions
		for (auto it = deletors.rbegin(); it != deletors.rend(); it++) {
			(*it)(); // call the function
		}

		deletors.clear();
	}
};

struct EnvironmentResources
{
	VkImageView environmentView{ VK_NULL_HANDLE }; // unblurred HDR/sky cubemap for background
	VkImageView irradianceView{ VK_NULL_HANDLE }; // diffuse irradiance cubemap
	VkImageView prefilterView{ VK_NULL_HANDLE }; // specular prefiltered cubemap 
	VkImageView brdfLutView{ VK_NULL_HANDLE }; // brdf lut 2D texture
	VkSampler cubemapSampler{ VK_NULL_HANDLE }; // environment and irradiance cubemap sampler
	VkSampler lutSampler{ VK_NULL_HANDLE }; // brdf lut sampler
};



// a buffer that has been allocated on the GPU
struct AllocatedBuffer
{
	VkBuffer buffer;
	VmaAllocation allocation;
	VmaAllocationInfo info;
};

// object data stored in gpu contiguous buffer
struct GPUObjectData
{
	glm::mat4 worldMatrix;
	glm::vec4 sphereBounds;
	VkDeviceAddress vertexBufferAddress;
	uint32_t batchIndex;
	uint32_t batchOffset;
};

constexpr uint32_t MAX_POINT_LIGHTS = 256;

struct GPUPointLight
{
	glm::vec4 position; // w component for light radius
	glm::vec4 color; // w component for light power
	glm::vec4 params; // x: cast shadow, yzw: padding
};

// per frame uniform data sent to GPU
struct GPUSceneData
{
	glm::mat4 view;
	glm::mat4 proj;
	glm::mat4 viewproj;
	glm::mat4 sunlightViewproj;
	glm::vec4 cameraPosition;
	glm::vec4 ambientColor;
	glm::vec4 sunlightDirection; // w component for sun power
	glm::vec4 sunlightColor;
	GPUPointLight pointLights[MAX_POINT_LIGHTS];
	glm::vec4 lightCount; // active point light count
};

// a single vertex 
struct Vertex
{
	glm::vec3 position;
	float uv_x;
	glm::vec3 normal;
	float uv_y;
	glm::vec4 color;
};

// used to identify pass groups for pipeline selection
enum class MaterialPassType : uint8_t
{
	Opaque,
	Transparent
};

// bounding volume of a mesh used for frustum culling
struct Bounds
{
	glm::vec3 origin{ 0.0f };
	float sphereRadius{ 0.0f };
	glm::vec3 extents{ 0.0f };
};

// a single renderable object in the scene
struct RenderObject
{
	uint32_t indexCount;
	uint32_t firstIndex;
	VkBuffer indexBuffer;

	VkDescriptorSet materialSet;
	MaterialPassType passType;
	Bounds bounds;
	glm::mat4 transform;
	VkDeviceAddress vertexBufferAddress;
};

// implementation of a scene node
// scene nodes can hold children and will also keep a transform to propagate to them
struct Node
{
	virtual ~Node() = default;

	// parent pointer must be a weak pointer to avoid circular dependencies
	std::weak_ptr<Node> parent;
	std::vector<std::shared_ptr<Node>> children;

	glm::mat4 localTransform;
	glm::mat4 worldTransform;

	void refreshTransform(const glm::mat4& parentMatrix)
	{
		worldTransform = parentMatrix * localTransform;
		for (auto c : children) {
			c->refreshTransform(worldTransform);
		}
	}
};

#define VK_CHECK(x)                                                     \
    do {                                                                \
        VkResult err = x;                                               \
        if (err) {                                                      \
            fmt::println("Detected Vulkan error: {}", string_VkResult(err)); \
            abort();                                                    \
        }                                                               \
    } while (0)