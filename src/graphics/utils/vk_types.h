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
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <core/DeletionQueue.h>

struct EnvironmentResources
{
	VkImageView environmentView{ VK_NULL_HANDLE }; // unblurred HDR/sky cubemap for background
	VkImageView irradianceView{ VK_NULL_HANDLE }; // diffuse irradiance cubemap
	VkImageView prefilterView{ VK_NULL_HANDLE }; // specular prefiltered cubemap 
	VkImageView brdfLutView{ VK_NULL_HANDLE }; // brdf lut 2D texture
	VkSampler cubemapSampler{ VK_NULL_HANDLE }; // environment and irradiance cubemap sampler
	VkSampler lutSampler{ VK_NULL_HANDLE }; // brdf lut sampler
};

struct TextureDescription {
	VkExtent3D extent{ 1, 1, 1 };
	VkFormat format{ VK_FORMAT_UNDEFINED };
	VkImageUsageFlags usage{ 0 };
	uint32_t mipLevels{ 1 };
	uint32_t arrayLayers{ 1 };
	VkSampleCountFlagBits samples{ VK_SAMPLE_COUNT_1_BIT };
	VkImageCreateFlags flags{ 0 };
	VkImageViewType viewType{ VK_IMAGE_VIEW_TYPE_2D };
	VkImageAspectFlags aspectMask = static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT);
	VkFilter magFilter{ VK_FILTER_LINEAR };
	VkFilter minFilter{ VK_FILTER_LINEAR };
	VkSamplerMipmapMode mipmapMode{ VK_SAMPLER_MIPMAP_MODE_LINEAR };
	VkSamplerAddressMode addressModeU{ VK_SAMPLER_ADDRESS_MODE_REPEAT };
	VkSamplerAddressMode addressModeV{ VK_SAMPLER_ADDRESS_MODE_REPEAT };
	VkSamplerAddressMode addressModeW{ VK_SAMPLER_ADDRESS_MODE_REPEAT };
};

// buffer data for a single renderable object
struct GPUObjectData
{
	glm::mat4 worldMatrix;
	glm::vec4 sphereBounds;
	VkDeviceAddress vertexBufferAddress; // address of the vertex buffer for this object
	uint32_t batchIndex;
	uint32_t batchOffset;
	uint32_t materialIndex;

	uint32_t padding[3]; 
};

constexpr uint32_t MAX_POINT_LIGHTS = 256;

struct GPUPointLight
{
	glm::vec4 position; // w component for light radius
	glm::vec4 color; // w component for light power
	glm::vec4 params; // x: cast shadow, yzw: padding
};

// uniform buffer data for the scene 
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

// data that is sent to shader for a material instance
struct MaterialConstants
{
	glm::vec4 colorFactors;
	glm::vec4 metalRoughFactors;
	glm::vec4 extraFactors; // x: occlusionStrength, y: normalScale, z, isDoubleSided
	glm::vec4 emissiveFactor; // xyz: emissiveFactor, w: emissiveStrength
	uint32_t albedoIndex; // index in bindless texture array for albedo texture
	uint32_t normalIndex; // index in bindless texture array for normal texture
	uint32_t metalRoughIndex; // index in bindless texture array for metal/roughness texture
	uint32_t occlusionIndex; // index in bindless texture array for occlusion texture
	uint32_t emissiveIndex; // index in bindless texture array for emissive texture

	uint32_t padding[3];
};

// a single vertex 
struct Vertex
{
	glm::vec3 position;
	float uv_x;
	glm::vec3 normal;
	float uv_y;
	glm::vec4 color;
	glm::vec4 tangent;
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
	uint32_t materialIndex; // index into the bindless material buffer for this object
	MaterialPassType passType;
	Bounds bounds;
	glm::mat4 transform;
	VkDeviceAddress vertexBufferAddress;
	bool isDoubleSided{ false };
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