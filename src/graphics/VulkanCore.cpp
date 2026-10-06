#include "VulkanCore.h"
#include "VkBootstrap.h"
#include "utils/vk_initializers.h"
#include "utils/vk_utils.h"
#include <glm/gtx/transform.hpp>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

constexpr bool bUseValidationLayers = true;

void VulkanCore::init(Window& window)
{
	vkb::InstanceBuilder builder;

	// make the vulkan insntace, with baisc debug features
	auto inst_ret = builder.set_app_name("Vulkan Application")
		.request_validation_layers(bUseValidationLayers)
		.use_default_debug_messenger()
		.require_api_version(1, 3, 0)
		.build();

	vkb::Instance vkb_inst = inst_ret.value();

	// grab the instance
	_instance = vkb_inst.instance;
	_debug_messenger = vkb_inst.debug_messenger;

	// create a surface that we can render to
	window.create_surface(_instance, &_surface);

	// vulkan 1.3 features
	VkPhysicalDeviceVulkan13Features features{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
	features.dynamicRendering = true;
	features.synchronization2 = true;

	// vulkan 1.2 features
	VkPhysicalDeviceVulkan12Features features12{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
	features12.bufferDeviceAddress = true;
	features12.descriptorIndexing = true;
	features12.drawIndirectCount = true;

	VkPhysicalDeviceFeatures standardFeatures{};
	standardFeatures.multiDrawIndirect = true;

	// use vkbootstrap to select a gpu
	// choose gpu that can write to the SDL surface and supports vulkan 1.3 with correct features
	vkb::PhysicalDeviceSelector selector{ vkb_inst };
	vkb::PhysicalDevice physicalDevice = selector
		.set_minimum_version(1, 3)
		.set_required_features_13(features)
		.set_required_features_12(features12)
		.set_required_features(standardFeatures)
		.set_surface(_surface)
		.select()
		.value();

	// create final vulkan device
	vkb::DeviceBuilder deviceBuilder{ physicalDevice };
	vkb::Device vkbDevice = deviceBuilder.build().value();

	// get VkDevice handle used in rest of vulkan application
	_device = vkbDevice.device;
	_chosenGPU = physicalDevice.physical_device;

	// use vkbootstrap to get a graphics queue
	_graphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
	_graphicsQueueFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

	// initialize memory allocator
	VmaAllocatorCreateInfo allocatorInfo = {};
	allocatorInfo.physicalDevice = _chosenGPU;
	allocatorInfo.device = _device;
	allocatorInfo.instance = _instance;
	allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	vmaCreateAllocator(&allocatorInfo, &_allocator);

	init_immediate_submit();

	init_default_data();
}

void VulkanCore::cleanup()
{
	// destroy samplers and images
	vkDestroySampler(_device, _shadowSampler, nullptr);
	vkDestroySampler(_device, _defaultSamplerNearest, nullptr);
	vkDestroySampler(_device, _defaultSamplerLinear, nullptr);

	// destroy default images
	_whiteImage.unload(this);
	_greyImage.unload(this);
	_blackImage.unload(this);
	_errorCheckerboardImage.unload(this);
	_defaultNormalImage.unload(this);

	// destroy immediate submit resources
	vkDestroyFence(_device, _immFence, nullptr);
	vkDestroyCommandPool(_device, _immCommandPool, nullptr);

	// destroy memory allocator
	vmaDestroyAllocator(_allocator);

	// destroy vulkan resources
	vkDestroySurfaceKHR(_instance, _surface, nullptr);
	vkDestroyDevice(_device, nullptr);
	vkb::destroy_debug_utils_messenger(_instance, _debug_messenger);
	vkDestroyInstance(_instance, nullptr);
}

void VulkanCore::immediate_submit(std::function<void(VkCommandBuffer cmd)>&& function)
{
	VK_CHECK(vkResetFences(_device, 1, &_immFence));
	VK_CHECK(vkResetCommandBuffer(_immCommandBuffer, 0));

	VkCommandBuffer cmd = _immCommandBuffer;

	VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

	VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

	function(cmd);

	VK_CHECK(vkEndCommandBuffer(cmd));

	VkCommandBufferSubmitInfo cmdInfo = vkinit::command_buffer_submit_info(cmd);
	VkSubmitInfo2 submit = vkinit::submit_info(&cmdInfo, nullptr, nullptr);

	// submit command buffer to the queue and execute it
	// _renderFence will now block until the graphics commands finish execution
	VK_CHECK(vkQueueSubmit2(_graphicsQueue, 1, &submit, _immFence));
	VK_CHECK(vkWaitForFences(_device, 1, &_immFence, true, 9999999999));
}


void VulkanCore::init_immediate_submit()
{
	// create a command pool for commands submitted to the graphics queue
	VkCommandPoolCreateInfo commandPoolInfo = vkinit::command_pool_create_info(_graphicsQueueFamily, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
	VK_CHECK(vkCreateCommandPool(_device, &commandPoolInfo, nullptr, &_immCommandPool));

	// allocate the default command buffer that we will use for rendering
	VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(_immCommandPool, 1);
	VK_CHECK(vkAllocateCommandBuffers(_device, &cmdAllocInfo, &_immCommandBuffer));

	// fence to control when the gpu has finished rendering the frame
	VkFenceCreateInfo fenceCreateInfo = vkinit::fence_create_info(VK_FENCE_CREATE_SIGNALED_BIT);
	VK_CHECK(vkCreateFence(_device, &fenceCreateInfo, nullptr, &_immFence));
}

void VulkanCore::init_default_data()
{
	// 3 default textures (white, grey, black) 1 pixel each
	uint32_t white = glm::packUnorm4x8(glm::vec4(1, 1, 1, 1));
	TextureDescription desc1{ .extent = {1, 1, 1}, .format = VK_FORMAT_R8G8B8A8_UNORM, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
	_whiteImage = Texture("default_white", desc1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&white), sizeof(uint32_t)));
	_whiteImage.load(this);

	uint32_t grey = glm::packUnorm4x8(glm::vec4(0.66f, 0.66f, 0.66f, 1.f));
	_greyImage = Texture("default_grey", desc1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&grey), sizeof(uint32_t)));
	_greyImage.load(this);

	uint32_t black = glm::packUnorm4x8(glm::vec4(0, 0, 0, 0));
	_blackImage = Texture("default_black", desc1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&black), sizeof(uint32_t)));
	_blackImage.load(this);

	uint32_t normal = glm::packUnorm4x8(glm::vec4(0.5f, 0.5f, 1.0f, 1.0f));
	_defaultNormalImage = Texture("default_normal", desc1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&normal), sizeof(uint32_t)));
	_defaultNormalImage.load(this);

	// checkerboard image
	uint32_t magenta = glm::packUnorm4x8(glm::vec4(1, 0, 1, 1));
	std::array<uint32_t, 16 * 16> pixels; // 16x16 checkerboard texture
	for (int x = 0; x < 16; x++) {
		for (int y = 0; y < 16; y++) {
			pixels[y * 16 + x] = ((x % 2) ^ (y % 2)) ? magenta : black;
		}
	}
	TextureDescription desc2{ .extent = {16, 16, 1}, .format = VK_FORMAT_R8G8B8A8_UNORM, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
	_errorCheckerboardImage = Texture("default_checkerboard", desc2, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(pixels.data()), sizeof(pixels)));
	_errorCheckerboardImage.load(this);

	VkSamplerCreateInfo samplerInfo = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samplerInfo.magFilter = VK_FILTER_NEAREST;
	samplerInfo.minFilter = VK_FILTER_NEAREST;
	vkCreateSampler(_device, &samplerInfo, nullptr, &_defaultSamplerNearest);

	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	vkCreateSampler(_device, &samplerInfo, nullptr, &_defaultSamplerLinear);

	VkSamplerCreateInfo shadowSamplerInfo{ .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	shadowSamplerInfo.magFilter = VK_FILTER_LINEAR;
	shadowSamplerInfo.minFilter = VK_FILTER_LINEAR;
	shadowSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	shadowSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	shadowSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	shadowSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	shadowSamplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
	vkCreateSampler(_device, &shadowSamplerInfo, nullptr, &_shadowSampler);
}
