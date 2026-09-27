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

// an image that has been allocated on the GPU
struct AllocatedImage
{
	VkImage image;
	VkImageView imageView;
	VmaAllocation allocation;
	VkExtent3D imageExtent;
	VkFormat imageFormat;
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

// per frame uniform data sent to GPU
struct GPUSceneData
{
	glm::mat4 view;
	glm::mat4 proj;
	glm::mat4 viewproj;
	glm::vec4 ambientColor;
	glm::vec4 sunlightDirection; // w component for sun power
	glm::vec4 sunlightColor;
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

// used to identify shading models for different passes
enum class MaterialType : uint8_t
{
	MetallicRoughness,
	Unlit
};

// used to identify pass groups for pipeline selection
enum class MaterialPassType : uint8_t
{
	Opaque,
	Transparent
};

struct MaterialInstance
{
	MaterialType type{ MaterialType::MetallicRoughness };
	MaterialPassType passType{ MaterialPassType::Opaque };
	VkDescriptorSet materialSet;
};

// resources needed for a mesh
struct GPUMeshBuffers
{
	AllocatedBuffer indexBuffer;
	AllocatedBuffer vertexBuffer;
	VkDeviceAddress vertexBufferAddress; 
};

// data that is pushed to the GPU for a draw call
struct GPUDrawPushConstants
{
	VkDeviceAddress vertexBufferAddress;
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

	MaterialInstance* material;
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