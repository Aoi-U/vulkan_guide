//> includes
#include "vk_engine.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <utils/vk_initializers.h>
#include <utils/vk_types.h>
#include <utils/vk_images.h>
#include <utils/vk_pipelines.h>

// bootstrap library
#include "VkBootstrap.h"

// imgui 
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_vulkan.h"

#include <glm/gtx/transform.hpp>

#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"

#include <chrono>
#include <thread>

VulkanEngine* loadedEngine = nullptr;

constexpr bool bUseValidationLayers = true;

VulkanEngine& VulkanEngine::Get() { return *loadedEngine; }
void VulkanEngine::init()
{
	// only one engine initialization is allowed with the application.
	assert(loadedEngine == nullptr);
	loadedEngine = this;

	// We initialize SDL and create a window with it.
	SDL_Init(SDL_INIT_VIDEO);

	SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);

	_window = SDL_CreateWindow(
		"Vulkan Engine",
		SDL_WINDOWPOS_UNDEFINED,
		SDL_WINDOWPOS_UNDEFINED,
		_windowExtent.width,
		_windowExtent.height,
		window_flags);

	init_vulkan();

	init_swapchain();

	init_commands();

	init_sync_structures();

	init_descriptors();

	init_pipelines();

	init_imgui();

	init_default_data();

	// everything went fine
	_isInitialized = true;

	mainCamera.velocity = glm::vec3(0.f);
	mainCamera.position = glm::vec3(30.f, 0.f, -085.f);
	mainCamera.pitch = 0;
	mainCamera.yaw = 0;

	//std::string structurePath = { "../../assets/structure.glb" };
	//auto structureFile = loadGltf(this, structurePath);
	//assert(structureFile.has_value());
	//loadedScenes["structure"] = *structureFile;

	std::string structurePath = { "../../assets/structure.glb" };
	auto structureFile = loadGltf(this, structurePath);
	assert(structureFile.has_value());
	loadedScenes["structure"] = *structureFile;
}

void VulkanEngine::cleanup()
{
	if (_isInitialized) {
		// make sure the gpu has stopped doing its things
		vkDeviceWaitIdle(_device);

		loadedScenes.clear();

		for (int i = 0; i < FRAME_OVERLAP; i++) {
			// flush the deletion queue
			_frames[i]._deletionQueue.flush();
		}

		metalRoughMaterial.clear_resources(_device);

		// flush global deletion queue
		_mainDeletionQueue.flush();

		_swapchain.cleanup(_device);

		vkDestroySurfaceKHR(_instance, _surface, nullptr);

		vkDestroyDevice(_device, nullptr);

		vkb::destroy_debug_utils_messenger(_instance, _debug_messenger);
		vkDestroyInstance(_instance, nullptr);

		SDL_DestroyWindow(_window);
	}

	// clear engine pointer
	loadedEngine = nullptr;
}

void VulkanEngine::draw()
{
	update_scene();

	// wait until the gpu has finished rendering the last frame
	// timeout of 1 second
	VK_CHECK(vkWaitForFences(_device, 1, &get_current_frame()._renderFence, true, 1000000000));

	get_current_frame()._deletionQueue.flush();
	get_current_frame()._frameDescriptors.clear_pools(_device);

	const auto& swapchainImages = _swapchain.get_images();
	const auto& swapchainImageViews = _swapchain.get_image_views();
	const auto& renderSemaphores = _swapchain.get_render_semaphores();
	const auto& swapchainExtent = _swapchain.get_extent();

	// request image from swapchain
	uint32_t swapchainImageIndex;
	VkResult e = vkAcquireNextImageKHR(_device, _swapchain.get(), 1000000000, get_current_frame()._swapchainSemaphore, nullptr, &swapchainImageIndex);
	if (e == VK_ERROR_OUT_OF_DATE_KHR) {
		resize_requested = true;
		return;
	}

	_drawExtent.height = std::min(swapchainExtent.height, _drawImage.imageExtent.height) * renderScale;
	_drawExtent.width = std::min(swapchainExtent.width, _drawImage.imageExtent.width) * renderScale;

	VK_CHECK(vkResetFences(_device, 1, &get_current_frame()._renderFence));

	// begin rendering commands
	VkCommandBuffer cmd = get_current_frame()._mainCommandBuffer;

	// now that we are sure the commands finished executing, we can safely reset the command buffer to begin recording again
	VK_CHECK(vkResetCommandBuffer(cmd, 0));

	// begin command buffer recording
	// we will use this command buffer exactly once, so we want to let vulkan know that
	VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

	// start the command buffer recording
	VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

	// transition our main draw image into general layout so we can write into it
	// overwrite it all so we dont care about what the older layout was
	vkutil::transition_image(cmd, _drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

	draw_background(cmd);

	vkutil::transition_image(cmd, _drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	vkutil::transition_image(cmd, _depthImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

	draw_geometry(cmd);

	// transition the draw image and the swapchain image into their correct transfer layouts 
	// so we can blit from the draw image to the swapchain image
	vkutil::transition_image(cmd, _drawImage.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	vkutil::transition_image(cmd, swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	// copy from draw image into the swapchain
	vkutil::copy_image_to_image(cmd, _drawImage.image, swapchainImages[swapchainImageIndex], _drawExtent, swapchainExtent);

	// set swapchain image layout to present so can show it on the screen
	vkutil::transition_image(cmd, swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

	// draw imgui on top of the swapchain image
	draw_imgui(cmd, swapchainImageViews[swapchainImageIndex]);

	// set swapchain image layout to Present so we can draw it
	vkutil::transition_image(cmd, swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

	// finalize the command buffer
	// can no longer add commands, but can now be executed
	VK_CHECK(vkEndCommandBuffer(cmd));

	// prepare the submission to the queue
	// want to wait on the _presentSemaphore, as that semaphore is signaled when the swapchain is ready
	// will signal the _renderSemaphore, to signal that rendering has finished
	VkCommandBufferSubmitInfo cmdInfo = vkinit::command_buffer_submit_info(cmd);

	VkSemaphoreSubmitInfo waitInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR, get_current_frame()._swapchainSemaphore);
	VkSemaphoreSubmitInfo signalInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, renderSemaphores[swapchainImageIndex]);

	VkSubmitInfo2 submit = vkinit::submit_info(&cmdInfo, &signalInfo, &waitInfo);

	// submit command buffer to the queue and execute it
	// _renderFence will now block until the graphics commands finish execution
	VK_CHECK(vkQueueSubmit2(_graphicsQueue, 1, &submit, get_current_frame()._renderFence));

	// prepare present
	// this will put the image we just rendered to the visible window
	// want to wait on the _renderSemaphore for that, as its necessary that drawing commands have finished before the image is displayed to the user
	VkSwapchainKHR swapchainHandle = _swapchain.get();
	VkPresentInfoKHR presentInfo = {};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.pNext = nullptr;
	presentInfo.pSwapchains = &swapchainHandle;
	presentInfo.swapchainCount = 1;

	presentInfo.pWaitSemaphores = &renderSemaphores[swapchainImageIndex];
	presentInfo.waitSemaphoreCount = 1;

	presentInfo.pImageIndices = &swapchainImageIndex;

	VkResult presentResult = vkQueuePresentKHR(_graphicsQueue, &presentInfo);
	if (presentResult == VK_ERROR_OUT_OF_DATE_KHR) {
		resize_requested = true;
		return;
	}

	// increase the number of frames drawn
	_frameNumber++;
}

void VulkanEngine::draw_background(VkCommandBuffer cmd)
{
	ComputeEffect& effect = backgroundEffects[currentBackgroundEffect];

	// bind the background compute pipeline
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, effect.pipeline);

	// bind the descriptor set containing the draw image for the compute pipeline
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _gradientPipelineLayout, 0, 1, &_drawImageDescriptorSet, 0, nullptr);

	vkCmdPushConstants(cmd, _gradientPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePushConstants), &effect.data);

	// execute the compute pipeline dispatch
	// using 16x16 workgroup size so we need to divide by it
	vkCmdDispatch(cmd, std::ceil(_drawExtent.width / 16.0), std::ceil(_drawExtent.height / 16.0), 1);
}

void VulkanEngine::draw_geometry(VkCommandBuffer cmd)
{
	// begin a render pass connected to our draw image
	VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(_drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(_depthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

	VkRenderingInfo renderInfo = vkinit::rendering_info(_drawExtent, &colorAttachment, &depthAttachment);

	vkCmdBeginRendering(cmd, &renderInfo);

	// allocate a new uniform buffer for scene data
	AllocatedBuffer gpuSceneDataBuffer = create_buffer(sizeof(GPUSceneData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	// add to deletion queue of this frame so it gets deleted once its been used
	get_current_frame()._deletionQueue.push_function([=, this]() {
		destroy_buffer(gpuSceneDataBuffer);
		});

	// write the buffer
	GPUSceneData* sceneUniformData = (GPUSceneData*)gpuSceneDataBuffer.allocation->GetMappedData();
	*sceneUniformData = sceneData;

	// create a descriptor set that binds that buffer and update it
	VkDescriptorSet globalDescriptor = get_current_frame()._frameDescriptors.allocate(_device, _gpuSceneDataDescriptorLayout);

	DescriptorWriter writer;
	writer.write_buffer(0, gpuSceneDataBuffer.buffer, sizeof(GPUSceneData), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	writer.update_set(_device, globalDescriptor);

	auto draw = [&](const RenderObject& draw) {
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.material->pipeline->pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.material->pipeline->layout, 0, 1, &globalDescriptor, 0, nullptr);

		// set dynamic viewport and scissor
		VkViewport viewport = {};
		viewport.x = 0;
		viewport.y = 0;
		viewport.width = _windowExtent.width;
		viewport.height = _windowExtent.height;
		viewport.minDepth = 0.f;
		viewport.maxDepth = 1.f;

		vkCmdSetViewport(cmd, 0, 1, &viewport);

		VkRect2D scissor = {};
		scissor.offset.x = 0;
		scissor.offset.y = 0;
		scissor.extent.width = _windowExtent.width;
		scissor.extent.height = _windowExtent.height;

		vkCmdSetScissor(cmd, 0, 1, &scissor);

		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.material->pipeline->layout, 1, 1, &draw.material->materialSet, 0, nullptr);

		vkCmdBindIndexBuffer(cmd, draw.indexBuffer, 0, VK_INDEX_TYPE_UINT32);

		GPUDrawPushConstants pushConstants;
		pushConstants.vertexBufferAddress = draw.vertexBufferAddress;
		pushConstants.worldMatrix = draw.transform;
		vkCmdPushConstants(cmd, draw.material->pipeline->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GPUDrawPushConstants), &pushConstants);

		vkCmdDrawIndexed(cmd, draw.indexCount, 1, draw.firstIndex, 0, 0);
		};

	for (auto& r : mainDrawContext.opaqueSurfaces) {
		draw(r);
	}

	for (auto& r : mainDrawContext.transparentSurfaces) {
		draw(r);
	}

	mainDrawContext.opaqueSurfaces.clear();
	mainDrawContext.transparentSurfaces.clear();

	vkCmdEndRendering(cmd);
}

void VulkanEngine::draw_imgui(VkCommandBuffer cmd, VkImageView targetImageView)
{
	VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(targetImageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingInfo renderInfo = vkinit::rendering_info(_swapchain.get_extent(), &colorAttachment, nullptr);

	vkCmdBeginRendering(cmd, &renderInfo);

	ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

	vkCmdEndRendering(cmd);
}

void VulkanEngine::update_scene()
{
	static auto lastTime = std::chrono::steady_clock::now();
	auto now = std::chrono::steady_clock::now();
	float deltaTime = std::chrono::duration<float>(now - lastTime).count();
	lastTime = now;

	mainCamera.update(deltaTime);

	// camera projection
	sceneData.proj = glm::perspective(glm::radians(70.0f), (float)_drawExtent.width / (float)_drawExtent.height, 10000.f, 0.1f);

	// invert y direction on proj matrix so that we are more similar to opengl and gltf axis
	sceneData.proj[1][1] *= -1;
	sceneData.view = mainCamera.getViewMatrix();
	sceneData.viewproj = sceneData.proj * sceneData.view;

	// some default lighting parameters
	sceneData.ambientColor = glm::vec4(0.1f);
	sceneData.sunlightColor = glm::vec4(1.0f);
	sceneData.sunlightDirection = glm::vec4(0, 1, 0.5, 1);

	loadedScenes["structure"]->draw(glm::mat4{ 1.0 }, mainDrawContext);
}

void VulkanEngine::immediate_submit(std::function<void(VkCommandBuffer cmd)>&& function)
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

void VulkanEngine::run()
{
	SDL_Event e;
	bool bQuit = false;

	// main loop
	while (!bQuit) {

		// Handle events on queue
		while (SDL_PollEvent(&e) != 0) {
			// close the window when user alt-f4s or clicks the X button
			if (e.type == SDL_QUIT)
				bQuit = true;


			if (e.type == SDL_WINDOWEVENT) {
				if (e.window.event == SDL_WINDOWEVENT_MINIMIZED) {
					stop_rendering = true;
				}
				if (e.window.event == SDL_WINDOWEVENT_RESTORED) {
					stop_rendering = false;
				}
			}

			mainCamera.processSDLEvent(e);
			// send sdl event to imgui for handling
			ImGui_ImplSDL2_ProcessEvent(&e);
		}

		// do not draw if we are minimized
		if (stop_rendering) {
			// throttle the speed to avoid the endless spinning
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}

		if (resize_requested)
			resize_swapchain();

		// imgui new frame
		ImGui_ImplVulkan_NewFrame();
		ImGui_ImplSDL2_NewFrame();
		ImGui::NewFrame();

		if (ImGui::Begin("background")) {
			ImGui::SliderFloat("Render Scale", &renderScale, 0.3f, 1.f);
			ComputeEffect& selected = backgroundEffects[currentBackgroundEffect];
			ImGui::Text("Selected effect: ", selected.name);

			ImGui::SliderInt("Effect index", &currentBackgroundEffect, 0, backgroundEffects.size() - 1);

			ImGui::InputFloat4("data1", (float*)&selected.data.data1);
			ImGui::InputFloat4("data2", (float*)&selected.data.data2);
			ImGui::InputFloat4("data3", (float*)&selected.data.data3);
			ImGui::InputFloat4("data4", (float*)&selected.data.data4);
		}
		ImGui::End();

		// make imgui calculate internal draw structures
		ImGui::Render();

		draw();
	}
}

AllocatedBuffer VulkanEngine::create_buffer(size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage)
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

void VulkanEngine::destroy_buffer(const AllocatedBuffer& buffer)
{
	vmaDestroyBuffer(_allocator, buffer.buffer, buffer.allocation);
}

AllocatedImage VulkanEngine::create_image(VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped)
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

AllocatedImage VulkanEngine::create_image(void* data, VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped /* = false */)
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

		vkutil::transition_image(cmd, newImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		});

	// destroy upload buffer
	destroy_buffer(uploadBuffer);

	return newImage;
}

void VulkanEngine::destroy_image(const AllocatedImage& image)
{
	vkDestroyImageView(_device, image.imageView, nullptr);
	vmaDestroyImage(_allocator, image.image, image.allocation);
}

GPUMeshBuffers VulkanEngine::uploadMesh(std::span<uint32_t> indices, std::span<Vertex> vertices)
{
	const size_t vertexBufferSize = vertices.size() * sizeof(Vertex);
	const size_t indexBufferSize = indices.size() * sizeof(uint32_t);

	GPUMeshBuffers newSurface;

	// create vertex buffer
	newSurface.vertexBuffer = create_buffer(vertexBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

	// get the address of the vertex buffer
	VkBufferDeviceAddressInfo deviceAddressInfo = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
		.buffer = newSurface.vertexBuffer.buffer
	};
	newSurface.vertexBufferAddress = vkGetBufferDeviceAddress(_device, &deviceAddressInfo);

	// create index buffer
	newSurface.indexBuffer = create_buffer(indexBufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

	// create staging buffers for vertex and index data
	AllocatedBuffer staging = create_buffer(vertexBufferSize + indexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);

	void* data = staging.allocation->GetMappedData();

	// copy vertex buffer
	memcpy(data, vertices.data(), vertexBufferSize);
	// copy index buffer
	memcpy((char*)data + vertexBufferSize, indices.data(), indexBufferSize);

	// copy from staging buffer to gpu buffers
	immediate_submit([&](VkCommandBuffer cmd) {
		// copy vertex buffer
		VkBufferCopy vertexCopy = { 0 };
		vertexCopy.dstOffset = 0;
		vertexCopy.srcOffset = 0;
		vertexCopy.size = vertexBufferSize;

		vkCmdCopyBuffer(cmd, staging.buffer, newSurface.vertexBuffer.buffer, 1, &vertexCopy);

		// copy index buffer
		VkBufferCopy indexCopy = { 0 };
		indexCopy.dstOffset = 0;
		indexCopy.srcOffset = vertexBufferSize;
		indexCopy.size = indexBufferSize;

		vkCmdCopyBuffer(cmd, staging.buffer, newSurface.indexBuffer.buffer, 1, &indexCopy);
		});

	// destroy staging buffer
	destroy_buffer(staging);

	return newSurface;
}

void VulkanEngine::init_default_data()
{
	std::array<Vertex, 4> rectVertices;

	rectVertices[0].position = { 0.5,-0.5, 0 };
	rectVertices[1].position = { 0.5,0.5, 0 };
	rectVertices[2].position = { -0.5,-0.5, 0 };
	rectVertices[3].position = { -0.5,0.5, 0 };

	rectVertices[0].color = { 0,0, 0,1 };
	rectVertices[1].color = { 0.5,0.5,0.5 ,1 };
	rectVertices[2].color = { 1,0, 0,1 };
	rectVertices[3].color = { 0,1, 0,1 };

	rectVertices[0].uv_x = 1;
	rectVertices[0].uv_y = 0;
	rectVertices[1].uv_x = 0;
	rectVertices[1].uv_y = 0;
	rectVertices[2].uv_x = 1;
	rectVertices[2].uv_y = 1;
	rectVertices[3].uv_x = 0;
	rectVertices[3].uv_y = 1;

	std::array<uint32_t, 6> rectIndices;

	rectIndices[0] = 0;
	rectIndices[1] = 1;
	rectIndices[2] = 2;

	rectIndices[3] = 2;
	rectIndices[4] = 1;
	rectIndices[5] = 3;

	rectangle = uploadMesh(rectIndices, rectVertices);

	_mainDeletionQueue.push_function([&]() {
		destroy_buffer(rectangle.indexBuffer);
		destroy_buffer(rectangle.vertexBuffer);
		});

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
	vkCreateSampler(_device, &samplerInfo, nullptr, &_defaultSamplerNearest);

	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	vkCreateSampler(_device, &samplerInfo, nullptr, &_defaultSamplerLinear);

	_mainDeletionQueue.push_function([&]() {
		vkDestroySampler(_device, _defaultSamplerNearest, nullptr);
		vkDestroySampler(_device, _defaultSamplerLinear, nullptr);

		destroy_image(_whiteImage);
		destroy_image(_greyImage);
		destroy_image(_blackImage);
		destroy_image(_errorCheckerboardImage);
		});
}

void VulkanEngine::init_vulkan()
{
	vkb::InstanceBuilder builder;

	// make the vulkan instance, with basic debug features
	auto inst_ret = builder.set_app_name("Vulkan Application")
		.request_validation_layers(bUseValidationLayers)
		.use_default_debug_messenger()
		.require_api_version(1, 3, 0)
		.build();

	vkb::Instance vkb_inst = inst_ret.value();

	// grab the instance
	_instance = vkb_inst.instance;
	_debug_messenger = vkb_inst.debug_messenger;

	SDL_Vulkan_CreateSurface(_window, _instance, &_surface);

	// vulkan 1.3 features
	VkPhysicalDeviceVulkan13Features features{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
	features.dynamicRendering = true;
	features.synchronization2 = true;

	// vulkan 1.2 features
	VkPhysicalDeviceVulkan12Features features12{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
	features12.bufferDeviceAddress = true;
	features12.descriptorIndexing = true;

	// use vkbootstrap to select a gpu
	// choose gpu that can write to the SDL surface and supports vulkan 1.3 with correct features
	vkb::PhysicalDeviceSelector selector{ vkb_inst };
	vkb::PhysicalDevice physicalDevice = selector
		.set_minimum_version(1, 3)
		.set_required_features_13(features)
		.set_required_features_12(features12)
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

	_mainDeletionQueue.push_function([&]() {
		vmaDestroyAllocator(_allocator);
		});
}

void VulkanEngine::init_swapchain()
{
	_swapchain.init(_chosenGPU, _device, _surface, _windowExtent.width, _windowExtent.height);

	// create a draw image that we will render into, and then blit to the swapchain image

	// draw image size will match the window
	VkExtent3D drawImageExtent = {
		_windowExtent.width,
		_windowExtent.height,
		1
	};

	// hardcoding draw format to 32 bit float
	_drawImage.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
	_drawImage.imageExtent = drawImageExtent;

	VkImageUsageFlags drawImageUsages{};
	drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	drawImageUsages |= VK_IMAGE_USAGE_STORAGE_BIT;
	drawImageUsages |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

	VkImageCreateInfo rimg_info = vkinit::image_create_info(_drawImage.imageFormat, drawImageUsages, drawImageExtent);

	// for the draw image, we want to allocate it from gpu local memory
	VmaAllocationCreateInfo rimg_allocinfo = {};
	rimg_allocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	rimg_allocinfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	// allocate and create image
	vmaCreateImage(_allocator, &rimg_info, &rimg_allocinfo, &_drawImage.image, &_drawImage.allocation, nullptr);

	// build an image view for the draw image to use for rendering
	VkImageViewCreateInfo rview_info = vkinit::imageview_create_info(_drawImage.imageFormat, _drawImage.image, VK_IMAGE_ASPECT_COLOR_BIT);

	VK_CHECK(vkCreateImageView(_device, &rview_info, nullptr, &_drawImage.imageView));

	// create a depth image to use for depth testing
	_depthImage.imageFormat = VK_FORMAT_D32_SFLOAT;
	_depthImage.imageExtent = drawImageExtent;
	VkImageUsageFlags depthImageUsages{};
	depthImageUsages |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

	VkImageCreateInfo dimgInfo = vkinit::image_create_info(_depthImage.imageFormat, depthImageUsages, drawImageExtent);

	// allocate and create the image
	vmaCreateImage(_allocator, &dimgInfo, &rimg_allocinfo, &_depthImage.image, &_depthImage.allocation, nullptr);

	// build an image view for the depth image to use for rendering
	VkImageViewCreateInfo dviewInfo = vkinit::imageview_create_info(_depthImage.imageFormat, _depthImage.image, VK_IMAGE_ASPECT_DEPTH_BIT);

	VK_CHECK(vkCreateImageView(_device, &dviewInfo, nullptr, &_depthImage.imageView));

	// add to deletion queues
	_mainDeletionQueue.push_function([=]() {
		vkDestroyImageView(_device, _drawImage.imageView, nullptr);
		vmaDestroyImage(_allocator, _drawImage.image, _drawImage.allocation);

		vkDestroyImageView(_device, _depthImage.imageView, nullptr);
		vmaDestroyImage(_allocator, _depthImage.image, _depthImage.allocation);
		});

}

void VulkanEngine::init_commands()
{
	// create command pool for commands submitted to the graphics queue
	// also want the pool to allow for resetting of individual command buffers
	VkCommandPoolCreateInfo commandPoolInfo = vkinit::command_pool_create_info(_graphicsQueueFamily, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);

	for (int i = 0; i < FRAME_OVERLAP; i++) {
		VK_CHECK(vkCreateCommandPool(_device, &commandPoolInfo, nullptr, &_frames[i]._commandPool));

		// allocate the default command buffer that we will use for rendering
		VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(_frames[i]._commandPool, 1);

		VK_CHECK(vkAllocateCommandBuffers(_device, &cmdAllocInfo, &_frames[i]._mainCommandBuffer));

		_mainDeletionQueue.push_function([=]() {
			vkDestroyCommandPool(_device, _frames[i]._commandPool, nullptr);
			});
	}

	// create command pool for immediate submit
	VK_CHECK(vkCreateCommandPool(_device, &commandPoolInfo, nullptr, &_immCommandPool));

	// allocate the command buffer for immediate submit
	VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(_immCommandPool, 1);

	VK_CHECK(vkAllocateCommandBuffers(_device, &cmdAllocInfo, &_immCommandBuffer));

	_mainDeletionQueue.push_function([=]() {
		vkDestroyCommandPool(_device, _immCommandPool, nullptr);
		});
}

void VulkanEngine::init_sync_structures()
{
	// create syncronization structures
	// one fence to control when the gpu has finished rendering the frame
	// 2 semaphores to syncronize rendering with swapchain
	// want the fence to start signalled so we can wait on it on the first frame
	VkFenceCreateInfo fenceCreateInfo = vkinit::fence_create_info(VK_FENCE_CREATE_SIGNALED_BIT);
	VK_CHECK(vkCreateFence(_device, &fenceCreateInfo, nullptr, &_immFence));

	_mainDeletionQueue.push_function([=]() {
		vkDestroyFence(_device, _immFence, nullptr);
		});

	for (int i = 0; i < FRAME_OVERLAP; i++) {
		VK_CHECK(vkCreateFence(_device, &fenceCreateInfo, nullptr, &_frames[i]._renderFence));

		VkSemaphoreCreateInfo semaphoreCreateInfo = vkinit::semaphore_create_info();
		VK_CHECK(vkCreateSemaphore(_device, &semaphoreCreateInfo, nullptr, &_frames[i]._swapchainSemaphore));

		_mainDeletionQueue.push_function([=]() {
			vkDestroyFence(_device, _frames[i]._renderFence, nullptr);

			vkDestroySemaphore(_device, _frames[i]._swapchainSemaphore, nullptr);
			});
	}
}

void VulkanEngine::init_descriptors()
{
	// create a descriptor pool that will hold 10 sets with 1 image each
	std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> sizes =
	{
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 }
	};

	_globalDescriptorAllocator.init(_device, 10, sizes);

	// make the descriptor set layout for our compute draw
	{
		DescriptorLayoutBuilder builder;
		builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		_drawImageDescriptorLayout = builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
	}

	// make the descriptor set layout for gpu scene data
	{
		DescriptorLayoutBuilder builder;
		builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
		_gpuSceneDataDescriptorLayout = builder.build(_device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	}

	// make the descriptor set layout for single image sampler
	{
		DescriptorLayoutBuilder builder;
		builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
		_singleImageDescriptorLayout = builder.build(_device, VK_SHADER_STAGE_FRAGMENT_BIT);
	}

	// allocate a descriptor set for our draw image
	_drawImageDescriptorSet = _globalDescriptorAllocator.allocate(_device, _drawImageDescriptorLayout);

	{
		DescriptorWriter writer;
		writer.write_image(0, _drawImage.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		writer.update_set(_device, _drawImageDescriptorSet);
	}

	for (int i = 0; i < FRAME_OVERLAP; i++) {
		// create a descriptor pool
		std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> frameSizes = {
			{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3},
			{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
			{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3},
			{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}
		};

		_frames[i]._frameDescriptors = DescriptorAllocatorGrowable{};
		_frames[i]._frameDescriptors.init(_device, 1000, frameSizes);

		_mainDeletionQueue.push_function([&, i]() {
			_frames[i]._frameDescriptors.destroy_pools(_device);
			});
	}

	_mainDeletionQueue.push_function([&]() {
		_globalDescriptorAllocator.destroy_pools(_device);

		vkDestroyDescriptorSetLayout(_device, _drawImageDescriptorLayout, nullptr);
		vkDestroyDescriptorSetLayout(_device, _gpuSceneDataDescriptorLayout, nullptr);
		vkDestroyDescriptorSetLayout(_device, _singleImageDescriptorLayout, nullptr);
		});
}

void VulkanEngine::init_pipelines()
{
	// compute pipelines
	init_background_pipelines();

	// graphics pipelines
	metalRoughMaterial.build_pipelines(this);
}

void VulkanEngine::init_background_pipelines()
{
	// create the pipeline layout
	VkPipelineLayoutCreateInfo computeLayout{};
	computeLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	computeLayout.pNext = nullptr;
	computeLayout.pSetLayouts = &_drawImageDescriptorLayout;
	computeLayout.setLayoutCount = 1;

	// create push constant range for the compute shader
	VkPushConstantRange pushConstant{};
	pushConstant.offset = 0;
	pushConstant.size = sizeof(ComputePushConstants);
	pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	computeLayout.pPushConstantRanges = &pushConstant;
	computeLayout.pushConstantRangeCount = 1;

	VK_CHECK(vkCreatePipelineLayout(_device, &computeLayout, nullptr, &_gradientPipelineLayout));

	// create the compute pipeline object
	VkShaderModule gradientShader;
	if (!vkutil::load_shader_module("../../shaders/gradient_color.comp.spv", _device, &gradientShader)) {
		fmt::print("Error when building the compute shader\n");
	}

	VkShaderModule skyShader;
	if (!vkutil::load_shader_module("../../shaders/sky.comp.spv", _device, &skyShader)) {
		fmt::print("Error when building the compute shader\n");
	}

	VkPipelineShaderStageCreateInfo stageInfo{};
	stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageInfo.pNext = nullptr;
	stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stageInfo.module = gradientShader;
	stageInfo.pName = "main";

	VkComputePipelineCreateInfo computePipelineCreateInfo{};
	computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	computePipelineCreateInfo.pNext = nullptr;
	computePipelineCreateInfo.layout = _gradientPipelineLayout;
	computePipelineCreateInfo.stage = stageInfo;

	ComputeEffect gradient;
	gradient.layout = _gradientPipelineLayout;
	gradient.name = "gradient";
	gradient.data = {};

	// default colors
	gradient.data.data1 = glm::vec4(1, 0, 0, 1);
	gradient.data.data2 = glm::vec4(0, 0, 1, 1);

	VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &gradient.pipeline));

	// change the shader module only to create the sky shader
	computePipelineCreateInfo.stage.module = skyShader;

	ComputeEffect sky;
	sky.layout = _gradientPipelineLayout;
	sky.name = "sky";
	sky.data = {};

	// default sky parameters
	sky.data.data1 = glm::vec4(0.1, 0.2, 0.4, 0.97);

	VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &sky.pipeline));

	backgroundEffects.push_back(gradient);
	backgroundEffects.push_back(sky);

	// cleanup shader module
	vkDestroyShaderModule(_device, gradientShader, nullptr);
	vkDestroyShaderModule(_device, skyShader, nullptr);

	_mainDeletionQueue.push_function([=]() {
		vkDestroyPipelineLayout(_device, _gradientPipelineLayout, nullptr);
		vkDestroyPipeline(_device, sky.pipeline, nullptr);
		vkDestroyPipeline(_device, gradient.pipeline, nullptr);
		});
}

void VulkanEngine::init_imgui()
{
	// create descriptor pool for imgui
	// size of the pool is very oversize, but its copied from imgui demo 
	VkDescriptorPoolSize poolSizes[] = {
		{VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
		{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
	};

	VkDescriptorPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	poolInfo.maxSets = 1000;
	poolInfo.poolSizeCount = (uint32_t)std::size(poolSizes);
	poolInfo.pPoolSizes = poolSizes;

	VkDescriptorPool imguiPool;
	VK_CHECK(vkCreateDescriptorPool(_device, &poolInfo, nullptr, &imguiPool));

	// initialize imgui library

	// initialize core imgui structures
	ImGui::CreateContext();

	// initialize imgui for SDL
	ImGui_ImplSDL2_InitForVulkan(_window);

	// initialize imgui for vulkan
	ImGui_ImplVulkan_InitInfo initInfo = {};
	initInfo.Instance = _instance;
	initInfo.PhysicalDevice = _chosenGPU;
	initInfo.Device = _device;
	initInfo.Queue = _graphicsQueue;
	initInfo.DescriptorPool = imguiPool;
	initInfo.MinImageCount = 3;
	initInfo.ImageCount = 3;
	initInfo.UseDynamicRendering = true;

	// dynamic rendering parameters for imgui to use
	initInfo.PipelineRenderingCreateInfo = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	initInfo.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
	const auto& swapchainFormat = _swapchain.get_image_format();
	initInfo.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapchainFormat;

	initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

	ImGui_ImplVulkan_Init(&initInfo);
	ImGui_ImplVulkan_CreateFontsTexture();

	// add to deletion queue
	_mainDeletionQueue.push_function([=]() {
		ImGui_ImplVulkan_Shutdown();
		vkDestroyDescriptorPool(_device, imguiPool, nullptr);
		});

}

void VulkanEngine::resize_swapchain()
{
	vkDeviceWaitIdle(_device);

	_swapchain.destroy_swapchain();

	int w, h;
	SDL_GetWindowSize(_window, &w, &h);
	_windowExtent.width = w;
	_windowExtent.height = h;

	_swapchain.create_swapchain(_windowExtent.width, _windowExtent.height);

	resize_requested = false;
}

void GLTFMetallic_Roughness::build_pipelines(VulkanEngine* engine)
{
	VkShaderModule meshFragShader;
	if (!vkutil::load_shader_module("../../shaders/mesh.frag.spv", engine->_device, &meshFragShader)) {
		fmt::println("Error when building the mesh fragment shader module");

	}

	VkShaderModule meshVertexShader;
	if (!vkutil::load_shader_module("../../shaders/mesh.vert.spv", engine->_device, &meshVertexShader)) {
		fmt::println("Error when building the mesh vertex shader module");
	}

	// setup push constant range for transform matrix
	VkPushConstantRange matrixRange{};
	matrixRange.offset = 0;
	matrixRange.size = sizeof(GPUDrawPushConstants);
	matrixRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

	// build descriptor set layout for material
	DescriptorLayoutBuilder layoutBuilder;
	layoutBuilder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); // binding 0: uniform buffer for GLTFMaterialData
	layoutBuilder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 1: combined image sampler for base color texture
	layoutBuilder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 2: combined image sampler for metallic roughness texture

	materialLayout = layoutBuilder.build(engine->_device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

	// create pipeline layout for mesh pipeline
	VkDescriptorSetLayout layouts[] = { engine->_gpuSceneDataDescriptorLayout, materialLayout };

	VkPipelineLayoutCreateInfo meshLayoutInfo = vkinit::pipeline_layout_create_info();
	meshLayoutInfo.setLayoutCount = 2;
	meshLayoutInfo.pSetLayouts = layouts;
	meshLayoutInfo.pPushConstantRanges = &matrixRange;
	meshLayoutInfo.pushConstantRangeCount = 1;

	VkPipelineLayout newLayout;
	VK_CHECK(vkCreatePipelineLayout(engine->_device, &meshLayoutInfo, nullptr, &newLayout));

	opaquePipeline.layout = newLayout;
	transparentPipeline.layout = newLayout;

	// build the stage-create-info for both vertex and fragment stages
	// lets the pipeline know the shader modules per stage
	PipelineBuilder pipelineBuilder;
	pipelineBuilder.set_shaders(meshVertexShader, meshFragShader);
	pipelineBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	pipelineBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	pipelineBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	pipelineBuilder.set_multisampling_none();
	pipelineBuilder.disable_blending();
	pipelineBuilder.enable_depthtest(true, VK_COMPARE_OP_GREATER_OR_EQUAL);

	// render format
	pipelineBuilder.set_color_attachment_format(engine->_drawImage.imageFormat);
	pipelineBuilder.set_depth_format(engine->_depthImage.imageFormat);

	// use the pipeline layout we just created
	pipelineBuilder._pipelineLayout = newLayout;

	// build the opaque pipeline
	opaquePipeline.pipeline = pipelineBuilder.build_pipeline(engine->_device);

	// create the transparent pipeline
	// same as opaque but with blending enabled
	pipelineBuilder.enable_blending_additive();
	pipelineBuilder.enable_depthtest(false, VK_COMPARE_OP_GREATER_OR_EQUAL);

	transparentPipeline.pipeline = pipelineBuilder.build_pipeline(engine->_device);

	// clean up shader modules
	vkDestroyShaderModule(engine->_device, meshFragShader, nullptr);
	vkDestroyShaderModule(engine->_device, meshVertexShader, nullptr);
}

void GLTFMetallic_Roughness::clear_resources(VkDevice device)
{
	vkDestroyDescriptorSetLayout(device, materialLayout, nullptr);
	vkDestroyPipelineLayout(device, transparentPipeline.layout, nullptr);

	vkDestroyPipeline(device, transparentPipeline.pipeline, nullptr);
	vkDestroyPipeline(device, opaquePipeline.pipeline, nullptr);
}

MaterialInstance GLTFMetallic_Roughness::write_material(VkDevice device, MaterialPass pass, const MaterialResources& resources, DescriptorAllocatorGrowable& descriptorAllocator)
{
	// write the material data to the descriptor set
	MaterialInstance matData;
	matData.passType = pass;
	if (pass == MaterialPass::Transparent) {
		matData.pipeline = &transparentPipeline;
	}
	else {
		matData.pipeline = &opaquePipeline;
	}

	matData.materialSet = descriptorAllocator.allocate(device, materialLayout);

	writer.clear();
	writer.write_buffer(0, resources.dataBuffer, sizeof(MaterialConstants), resources.dataBufferOffset, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	writer.write_image(1, resources.colorImage.imageView, resources.colorSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.write_image(2, resources.metalRoughImage.imageView, resources.metalRoughSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

	writer.update_set(device, matData.materialSet);

	return matData;
}

void MeshNode::draw(const glm::mat4& topMatrix, DrawContext& ctx)
{
	glm::mat4 nodeMatrix = topMatrix * worldTransform;

	// draw all surfaces of the mesh
	// mesh can have multiple surfaces, each with its own material
	for (auto& s : mesh->surfaces) {
		RenderObject def;
		def.indexCount = s.count;
		def.firstIndex = s.startIndex;
		def.indexBuffer = mesh->meshBuffers.indexBuffer.buffer;
		def.material = &s.material->data;

		def.transform = nodeMatrix;
		def.vertexBufferAddress = mesh->meshBuffers.vertexBufferAddress;

		if (s.material->data.passType == MaterialPass::Transparent) {
			ctx.transparentSurfaces.push_back(def);
		}
		else {
			ctx.opaqueSurfaces.push_back(def);
		}
	}

	// recurse down
	Node::draw(topMatrix, ctx);
}
