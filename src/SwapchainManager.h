// class to manage vulkan swapchains
#include <utils/vk_types.h>

class SwapchainManager 
{
public:
	void init(VkPhysicalDevice chosenGPU, VkDevice device, VkSurfaceKHR surface, uint32_t width, uint32_t height);

	void cleanup(VkDevice device);
	
	void destroy_swapchain();

	void create_swapchain(uint32_t width, uint32_t height);

	// getters
	VkSwapchainKHR get_swapchain() const { return _swapchain; }
	std::vector<VkImage> get_swapchain_images() const { return _swapchainImages; }
	std::vector<VkSemaphore> get_render_semaphores() const { return _renderSemaphores; }
	VkFormat get_image_format() const { return _swapchainImageFormat; }
	VkExtent2D get_extent() const { return _swapchainExtent; }

private:
	// vulkan handles
	VkPhysicalDevice _chosenGPU;
	VkDevice _device;
	VkSurfaceKHR _surface;

	// swapchain handles
	VkSwapchainKHR _swapchain;
	VkFormat _swapchainImageFormat;
	std::vector<VkImage> _swapchainImages;
	std::vector<VkImageView> _swapchainImageViews;
	std::vector<VkSemaphore> _renderSemaphores;
	VkExtent2D _swapchainExtent;

};