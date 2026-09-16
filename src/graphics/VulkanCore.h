#include <core/vk_types.h>
#include <window/window.h>

class VulkanCore
{
public:
	void init(Window& window);

	void cleanup();

	VkInstance get_instance() const { return _instance; }
	VkPhysicalDevice get_physical_device() const { return _chosenGPU; }
	VkDevice get_device() const { return _device; }
	VkSurfaceKHR get_surface() const { return _surface; }

	VkQueue get_graphics_queue() const { return _graphicsQueue; }
	uint32_t get_graphics_queue_family() const { return _graphicsQueueFamily; }

	VmaAllocator& get_allocator() { return _allocator; }

private:
	// vulkan instance handles
	VkInstance _instance; // vulkan library handle
	VkDebugUtilsMessengerEXT _debug_messenger; // vulkan debug output handle
	VkPhysicalDevice _chosenGPU; // gpu chosen as the default device
	VkDevice _device; // vulkan logical device for comands
	VkSurfaceKHR _surface; // vulkan window surface

	VkQueue _graphicsQueue;
	uint32_t _graphicsQueueFamily;

	VmaAllocator _allocator;

};