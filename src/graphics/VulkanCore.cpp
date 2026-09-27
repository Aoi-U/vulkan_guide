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
	// destroy default samplers
	vkDestroySampler(_device, _defaultSamplerNearest, nullptr);
	vkDestroySampler(_device, _defaultSamplerLinear, nullptr);
	destroy_image(_whiteImage);
	destroy_image(_greyImage);
	destroy_image(_blackImage);
	destroy_image(_errorCheckerboardImage);

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

void VulkanCore::check_and_resize_buffer(AllocatedBuffer& buffer, size_t requiredSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage)
{
	if (buffer.buffer != VK_NULL_HANDLE && buffer.info.size >= requiredSize) {
		return;
	}

	size_t oldSize = buffer.buffer != VK_NULL_HANDLE ? buffer.info.size : 0;
	if (buffer.buffer != VK_NULL_HANDLE) {
		destroy_buffer(buffer);
		buffer = {};
	}

	size_t newSize = std::max(requiredSize, static_cast<size_t>(oldSize * 1.5));
	buffer = create_buffer(newSize, usage, memoryUsage);
}


AllocatedBuffer VulkanCore::create_buffer(size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage)
{
	// allocate buffer
	VkBufferCreateInfo bufferInfo = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bufferInfo.pNext = nullptr;
	bufferInfo.size = allocSize;

	bufferInfo.usage = usage;

	VmaAllocationCreateInfo vmaAllocInfo = {};
	vmaAllocInfo.usage = memoryUsage;
	vmaAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

	AllocatedBuffer newBuffer;
	// allocate the buffer
	VK_CHECK(vmaCreateBuffer(_allocator, &bufferInfo, &vmaAllocInfo, &newBuffer.buffer, &newBuffer.allocation, &newBuffer.info));

	return newBuffer;
}

void VulkanCore::destroy_buffer(const AllocatedBuffer& buffer)
{
	vmaDestroyBuffer(_allocator, buffer.buffer, buffer.allocation);
}

AllocatedImage VulkanCore::create_image(VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped)
{
	AllocatedImage newImage;
	newImage.imageFormat = format;
	newImage.imageExtent = size;

	VkImageCreateInfo imgInfo = vkinit::image_create_info(format, usage, size);
	if (mipmapped) {
		imgInfo.mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(size.width, size.height)))) + 1;
	}

	// always allocate images on dedicated GPU memory
	VmaAllocationCreateInfo allocInfo = {};
	allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	allocInfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	// allocate and create the image
	VK_CHECK(vmaCreateImage(_allocator, &imgInfo, &allocInfo, &newImage.image, &newImage.allocation, nullptr));

	// if depth format, use depth aspect, color aspect otherwise
	VkImageAspectFlags aspectFlag = format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

	// build an image view for the image
	VkImageViewCreateInfo viewInfo = vkinit::imageview_create_info(format, newImage.image, aspectFlag);
	viewInfo.subresourceRange.levelCount = imgInfo.mipLevels;

	VK_CHECK(vkCreateImageView(_device, &viewInfo, nullptr, &newImage.imageView));

	return newImage;
}

AllocatedImage VulkanCore::create_image(void* data, VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped /* = false */)
{
	size_t dataSize = size.depth * size.width * size.height * 4;
	AllocatedBuffer uploadBuffer = create_buffer(dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	// copy data into upload buffer
	memcpy(uploadBuffer.info.pMappedData, data, dataSize);

	AllocatedImage newImage = create_image(size, format, usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, mipmapped);

	immediate_submit([&](VkCommandBuffer cmd) {
		vkutil::transition_image(cmd, newImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		VkBufferImageCopy copyRegion = {};
		copyRegion.bufferOffset = 0;
		copyRegion.bufferRowLength = 0;
		copyRegion.bufferImageHeight = 0;

		copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.imageSubresource.mipLevel = 0;
		copyRegion.imageSubresource.baseArrayLayer = 0;
		copyRegion.imageSubresource.layerCount = 1;
		copyRegion.imageExtent = size;

		// copy from staging buffer to image
		vkCmdCopyBufferToImage(cmd, uploadBuffer.buffer, newImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

		if (mipmapped) {
			vkutil::generate_mipmaps(cmd, newImage.image, VkExtent2D{ newImage.imageExtent.width, newImage.imageExtent.height });
		}
		else {
			vkutil::transition_image(cmd, newImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}
		});

	// destroy upload buffer
	destroy_buffer(uploadBuffer);

	return newImage;
}

void VulkanCore::destroy_image(const AllocatedImage& image)
{
	vkDestroyImageView(_device, image.imageView, nullptr);
	vmaDestroyImage(_allocator, image.image, image.allocation);
}

GPUMeshBuffers VulkanCore::upload_mesh(std::span<uint32_t> indices, std::span<Vertex> vertices)
{
	const size_t vertexBufferSize = vertices.size() * sizeof(Vertex);
	const size_t indexBufferSize = indices.size() * sizeof(uint32_t);

	GPUMeshBuffers newMesh;

	// create vertex buffer
	newMesh.vertexBuffer = create_buffer(vertexBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

	// get the address of the vertex buffer
	VkBufferDeviceAddressInfo deviceAddressInfo = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
		.buffer = newMesh.vertexBuffer.buffer
	};
	newMesh.vertexBufferAddress = vkGetBufferDeviceAddress(_device, &deviceAddressInfo);

	// create index buffer
	newMesh.indexBuffer = create_buffer(indexBufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

	// create staging buffers for vertex and index data
	AllocatedBuffer staging = create_buffer(vertexBufferSize + indexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);

	void* data = staging.info.pMappedData;

	// copy vertex buffer
	memcpy(data, vertices.data(), vertexBufferSize);
	// copy index buffer
	memcpy((char*)data + vertexBufferSize, indices.data(), indexBufferSize);

	// copy from staging buffer to gpu buffers
	immediate_submit([&](VkCommandBuffer cmd) {
		// copy vertex buffer
		VkBufferCopy vertexCopy = { 
			.srcOffset = 0,
			.dstOffset = 0,
			.size = vertexBufferSize
		};

		vkCmdCopyBuffer(cmd, staging.buffer, newMesh.vertexBuffer.buffer, 1, &vertexCopy);

		// copy index buffer
		VkBufferCopy indexCopy = { 
			.srcOffset = vertexBufferSize,
			.dstOffset = 0,
			.size = indexBufferSize
		};

		vkCmdCopyBuffer(cmd, staging.buffer, newMesh.indexBuffer.buffer, 1, &indexCopy);
		});

	// destroy staging buffer
	destroy_buffer(staging);

	return newMesh;
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
	_whiteImage = create_image((void*)&white, VkExtent3D{ 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);

	uint32_t grey = glm::packUnorm4x8(glm::vec4(0.66f, 0.66f, 0.66f, 1.f));
	_greyImage = create_image((void*)&grey, VkExtent3D{ 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);

	uint32_t black = glm::packUnorm4x8(glm::vec4(0, 0, 0, 0));
	_blackImage = create_image((void*)&black, VkExtent3D{ 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);

	// checkerboard image
	uint32_t magenta = glm::packUnorm4x8(glm::vec4(1, 0, 1, 1));
	std::array<uint32_t, 16 * 16> pixels; // 16x16 checkerboard texture
	for (int x = 0; x < 16; x++) {
		for (int y = 0; y < 16; y++) {
			pixels[y * 16 + x] = ((x % 2) ^ (y % 2)) ? magenta : black;
		}
	}
	_errorCheckerboardImage = create_image(pixels.data(), VkExtent3D{ 16, 16, 1 }, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);

	VkSamplerCreateInfo samplerInfo = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samplerInfo.magFilter = VK_FILTER_NEAREST;
	samplerInfo.minFilter = VK_FILTER_NEAREST;
	vkCreateSampler(get_device(), &samplerInfo, nullptr, &_defaultSamplerNearest);

	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	vkCreateSampler(get_device(), &samplerInfo, nullptr, &_defaultSamplerLinear);
}
