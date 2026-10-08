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

struct DeletionQueue
{
public:
	void push(VkBuffer buffer, VmaAllocation allocation) { buffers.emplace_back(buffer, allocation); }
	void push(VkImage image, VmaAllocation allocation) { images.emplace_back(image, allocation); }
	void push(VkImageView imageView) { imageViews.push_back(imageView); }
	void push(VkSampler sampler) { samplers.push_back(sampler); }
	void push(VkShaderModule shaderModule) { shaderModules.push_back(shaderModule); }
	void push(VkPipeline pipeline) { pipelines.push_back(pipeline); }
	void push(VkPipelineLayout layout) { pipelineLayouts.push_back(layout); }
	void push(VkDescriptorSetLayout layout) { descriptorSetLayouts.push_back(layout); }

	void flush(VkDevice device, VmaAllocator allocator)
	{
		// Destroy handles in reverse dependency order
		for (auto it = imageViews.rbegin(); it != imageViews.rend(); ++it) {
			vkDestroyImageView(device, *it, nullptr);
		}
		imageViews.clear();

		for (auto it = samplers.rbegin(); it != samplers.rend(); ++it) {
			vkDestroySampler(device, *it, nullptr);
		}
		samplers.clear();

		for (auto it = images.rbegin(); it != images.rend(); ++it) {
			vmaDestroyImage(allocator, it->first, it->second);
		}
		images.clear();

		for (auto it = buffers.rbegin(); it != buffers.rend(); ++it) {
			vmaDestroyBuffer(allocator, it->first, it->second);
		}
		buffers.clear();

		for (auto it = shaderModules.rbegin(); it != shaderModules.rend(); ++it) {
			vkDestroyShaderModule(device, *it, nullptr);
		}
		shaderModules.clear();

		for (auto it = pipelines.rbegin(); it != pipelines.rend(); ++it) {
			vkDestroyPipeline(device, *it, nullptr);
		}
		pipelines.clear();

		for (auto it = pipelineLayouts.rbegin(); it != pipelineLayouts.rend(); ++it) {
			vkDestroyPipelineLayout(device, *it, nullptr);
		}
		pipelineLayouts.clear();

		for (auto it = descriptorSetLayouts.rbegin(); it != descriptorSetLayouts.rend(); ++it) {
			vkDestroyDescriptorSetLayout(device, *it, nullptr);
		}
		descriptorSetLayouts.clear();
	}

private:
	std::vector<std::pair<VkBuffer, VmaAllocation>> buffers;
	std::vector<std::pair<VkImage, VmaAllocation>> images;
	std::vector<VkImageView> imageViews;
	std::vector<VkSampler> samplers;
	std::vector<VkShaderModule> shaderModules;
	std::vector<VkPipeline> pipelines;
	std::vector<VkPipelineLayout> pipelineLayouts;
	std::vector<VkDescriptorSetLayout> descriptorSetLayouts;
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

	VkDescriptorSet materialSet;
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