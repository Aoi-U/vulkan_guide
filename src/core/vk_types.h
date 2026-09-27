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

// used to identify which pipeline to use for a given material instance
enum class MaterialPass : uint8_t
{
	MainColor,
	Transparent,
	Other
};

// a pipeline and its layout 
struct MaterialPipeline
{
	VkPipeline pipeline;
	VkPipelineLayout layout;
};

// a single instance of a material with its own resources and pipeline
struct MaterialInstance
{
	MaterialPipeline* pipeline;
	VkDescriptorSet materialSet;
	MaterialPass passType;
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

// object data stored in gpu contiguous buffer
struct GPUObjectData
{
	glm::mat4 worldMatrix;
	glm::vec4 sphereBounds;
	VkDeviceAddress vertexBufferAddress;
	uint32_t batchIndex;
	uint32_t batchOffset;
};

// frustum cull push constants
struct CullPushConstants
{
	glm::vec4 frustumPlanes[6]; 
	uint32_t drawCount; 
	uint32_t cullMode{ 1 }; // enable or disable
};

struct IndirectBatch
{
	VkBuffer indexBuffer;
	MaterialInstance* material;
	uint32_t first; // first index in the indirect buffer
	uint32_t count; // number of indirect commands in this batch
	uint32_t batchIndex; // index of this batch in the batch array
};

struct DrawContext
{
	std::vector<RenderObject> opaqueObjects; 
	std::vector<RenderObject> transparentObjects;

	std::vector<IndirectBatch> opaqueBatches;

	// indirect draw 
	uint32_t indirectDrawCount{ 0 };

	void clear() {
		opaqueObjects.clear();
		transparentObjects.clear();
		opaqueBatches.clear();
		
		indirectDrawCount = 0;
	}
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