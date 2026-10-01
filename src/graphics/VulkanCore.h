#pragma once
#include <core/vk_types.h>
#include <window/window.h>

struct ImageDescription {
	VkExtent3D extent{ 1, 1, 1 };
	VkFormat format{ VK_FORMAT_UNDEFINED };
	VkImageUsageFlags usage{ 0 };
	uint32_t mipLevels{ 1 };
	uint32_t arrayLayers{ 1 };
	VkSampleCountFlagBits samples{ VK_SAMPLE_COUNT_1_BIT };
	VkImageCreateFlags flags{ 0 };
	VkImageViewType viewType{ VK_IMAGE_VIEW_TYPE_2D };
	VkImageAspectFlags aspectMask{ VK_IMAGE_ASPECT_COLOR_BIT };
};

class VulkanCore
{
public:
	void init(Window& window);

	void cleanup();

	// submits a function to be executed immediately on the gpu
	void immediate_submit(std::function<void(VkCommandBuffer cmd)>&& function);	

	// resizes buffer if needed
	void check_and_resize_buffer(AllocatedBuffer& buffer, size_t requiredSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage);

	// creates a buffer 
	AllocatedBuffer create_buffer(size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage);
	void destroy_buffer(const AllocatedBuffer& buffer);

	// creates an image
	AllocatedImage create_image(VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped = false);

	// creates an image with data
	AllocatedImage create_image(void* data, VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped = false);

	// creates an image 
	AllocatedImage create_image(const ImageDescription& desc);

	void destroy_image(const AllocatedImage& image);

	VkInstance get_instance() const { return _instance; }
	VkPhysicalDevice get_physical_device() const { return _chosenGPU; }
	VkDevice get_device() const { return _device; }
	VkSurfaceKHR get_surface() const { return _surface; }

	VkQueue get_graphics_queue() const { return _graphicsQueue; }
	uint32_t get_graphics_queue_family() const { return _graphicsQueueFamily; }

	VmaAllocator& get_allocator() { return _allocator; }

	// default samplers and images
	// Default samplers and fallback images
	AllocatedImage _whiteImage;
	AllocatedImage _blackImage;
	AllocatedImage _greyImage;
	AllocatedImage _errorCheckerboardImage;
	VkSampler _defaultSamplerLinear{ VK_NULL_HANDLE };
	VkSampler _defaultSamplerNearest{ VK_NULL_HANDLE };

	// uploads a mesh to the gpu and returns the buffers for it
	GPUMeshBuffers upload_mesh(std::span<uint32_t> indices, std::span<Vertex> vertices);

private:
	void init_immediate_submit();
	void init_default_data();

	// vulkan instance handles
	VkInstance _instance; // vulkan library handle
	VkDebugUtilsMessengerEXT _debug_messenger; // vulkan debug output handle
	VkPhysicalDevice _chosenGPU; // gpu chosen as the default device
	VkDevice _device; // vulkan logical device for comands
	VkSurfaceKHR _surface; // vulkan window surface

	VkQueue _graphicsQueue;
	uint32_t _graphicsQueueFamily;

	VmaAllocator _allocator;

	VkFence _immFence;
	VkCommandBuffer _immCommandBuffer;
	VkCommandPool _immCommandPool;

};