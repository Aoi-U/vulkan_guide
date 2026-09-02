// vulkan_guide.h : Include file for standard system include files,
// or project specific include files.

#pragma once

#include <vk_types.h>

class VulkanEngine {
public:

	bool _isInitialized{ false };
	int _frameNumber{ 0 };
	bool stop_rendering{ false };
	VkExtent2D _windowExtent{ 1700 , 900 };

	// vulkan instance handles
	VkInstance _instance; // vulkan library handle
	VkDebugUtilsMessengerEXT _debug_messenger; // vulkan debug output handle
	VkPhysicalDevice _chosenGPU; // gpu chosen as the default device
	VkDevice _device; // vulkan logical device for comands
	VkSurfaceKHR _surface; // vulkan window surface

	// vulkan swapchain handles
	VkSwapchainKHR _swapchain;
	VkFormat _swapchainImageFormat;
	std::vector<VkImage> _swapchainImages;
	std::vector<VkImageView> _swapchainImageViews;
	VkExtent2D _swapchainExtent;

	struct SDL_Window* _window{ nullptr };

	static VulkanEngine& Get();

	//initializes everything in the engine
	void init();

	//shuts down the engine
	void cleanup();

	//draw loop
	void draw();

	//run main loop
	void run();

private:

	// vulkan initialization functions
	void init_vulkan();
	void init_swapchain();
	void init_commands();
	void init_sync_structures();

	// vulkan swapchain functions
	void create_swapchain(uint32_t width, uint32_t height);
	void destroy_swapchain();

};
