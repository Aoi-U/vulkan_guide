#include "SwapchainManager.h"

#include <utils/vk_initializers.h>

// bootstrap library
#include "VkBootstrap.h"

void SwapchainManager::init(VkPhysicalDevice chosenGPU, VkDevice device, VkSurfaceKHR surface, uint32_t width, uint32_t height)
{
	// store vulkan handles
	_chosenGPU = chosenGPU;
	_device = device;
	_surface = surface;

	create_swapchain(width, height);
}

void SwapchainManager::cleanup(VkDevice device)
{
	destroy_swapchain();
}

void SwapchainManager::destroy_swapchain()
{
	// destroy semaphores
	for (VkSemaphore semaphore : _renderSemaphores) {
		vkDestroySemaphore(_device, semaphore, nullptr);
	}
	_renderSemaphores.clear();

	vkDestroySwapchainKHR(_device, _swapchain, nullptr);

	// destroy swapchain resources
	for (VkImageView imageView : _swapchainImageViews) {
		vkDestroyImageView(_device, imageView, nullptr);
	}
}

void SwapchainManager::create_swapchain(uint32_t width, uint32_t height)
{
	vkb::SwapchainBuilder swapchainBuilder{ _chosenGPU, _device, _surface };
	_swapchainImageFormat = VK_FORMAT_B8G8R8A8_UNORM;

	vkb::Swapchain vkbSwapchain = swapchainBuilder
		.set_desired_format(VkSurfaceFormatKHR{ .format = _swapchainImageFormat, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
		// use vsync present mode
		.set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
		.set_desired_extent(width, height)
		.add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
		.build()
		.value();

	_swapchainExtent = vkbSwapchain.extent;

	// store swapchain and its related images
	_swapchain = vkbSwapchain.swapchain;
	_swapchainImages = vkbSwapchain.get_images().value();
	_swapchainImageViews = vkbSwapchain.get_image_views().value();

	VkSemaphoreCreateInfo semaphoreCreateInfo = vkinit::semaphore_create_info();
	_renderSemaphores.resize(_swapchainImages.size());
	for (int i = 0; i < _renderSemaphores.size(); i++) {
		VK_CHECK(vkCreateSemaphore(_device, &semaphoreCreateInfo, nullptr, &_renderSemaphores[i]));
	}
}
