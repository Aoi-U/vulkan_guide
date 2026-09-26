#include "Renderer.h"
#include <utils/vk_initializers.h>
#include <utils/vk_utils.h>
#include <cmath>
#include <algorithm>
#include <resources/vk_loader.h>
#include <core/Components.h>

#include <graphics/pass/FrustumCullpass.h>


void Renderer::init(Window* window, VulkanCore* vulkanCore)
{
	_window = window;
	_vulkanCore = vulkanCore;

	recreate_swapchain();
	create_command_buffers();
	create_sync_structures();

	//_meshRenderer = std::make_unique<MeshRenderer>();
	//_meshRenderer->init(_vulkanCore, _drawImage.imageFormat, _depthImage.imageFormat);

	std::unique_ptr<FrustumCullPass> frustumCullPass = std::make_unique<FrustumCullPass>();
	frustumCullPass->init(_vulkanCore, _drawImage.imageFormat, _depthImage.imageFormat);
	_computePasses.push_back(std::move(frustumCullPass));

	std::unique_ptr<ForwardPass> forwardPass = std::make_unique<ForwardPass>();
	forwardPass->init(_vulkanCore, _drawImage.imageFormat, _depthImage.imageFormat);
	_renderPasses.push_back(std::move(forwardPass));
	_forwardPass = static_cast<ForwardPass*>(_renderPasses.back().get());
}

void Renderer::cleanup()
{
	vkDeviceWaitIdle(_vulkanCore->get_device());

	//_meshRenderer->cleanup();
	for (auto& pass : _renderPasses) {
		pass->cleanup();
	}

	for (auto& pass : _computePasses) {
		pass->cleanup();
	}

	destroy_render_targets();

	for (int i = 0; i < FRAME_OVERLAP; i++) {
		_vulkanCore->destroy_buffer(_frames[i]._cullStatsBuffer);

		_frames[i]._deletionQueue.flush();
		_frames[i]._frameDescriptors.destroy_pools(_vulkanCore->get_device());

		vkDestroyCommandPool(_vulkanCore->get_device(), _frames[i]._commandPool, nullptr);
		vkDestroyFence(_vulkanCore->get_device(), _frames[i]._renderFence, nullptr);
		vkDestroySemaphore(_vulkanCore->get_device(), _frames[i]._swapchainSemaphore, nullptr);
	}

	_swapchain.cleanup(_vulkanCore->get_device());
}

/*
void Renderer::render_scene(const entt::registry& registry, const GPUSceneData& sceneData, GuiDrawFunction guiDrawFunction)
{
	if (VkCommandBuffer cmd = begin_frame()) {
		begin_rendering(cmd);
		// render mesh
		_meshRenderer->render(cmd, get_current_frame(), registry, sceneData, _drawExtent);

		end_rendering(cmd);

		blit_to_swapchain(cmd);

		// render gui
		if (guiDrawFunction) {
			VkImageView swapchainImageView = _swapchain.get_image_views()[_swapchainImageIndex];

			guiDrawFunction(cmd, swapchainImageView, _drawExtent);
		}

		end_frame();
	}
}
*/

void Renderer::render_frame(const entt::registry& registry, const GPUSceneData& sceneData, GuiDrawFunction guiDrawFunction)
{
	DrawContext ctx;
	extract_scene_data(registry, ctx);

	if (VkCommandBuffer cmd = begin_frame()) {
		for (auto& pass : _computePasses) {
			pass->render(cmd, get_current_frame(), ctx, sceneData, _drawExtent);
		}

		begin_rendering(cmd);
		
		// forward main pass
		//_forwardPass->render(cmd, get_current_frame(), ctx, sceneData, _drawExtent);
		for (auto& pass : _renderPasses) {
			pass->render(cmd, get_current_frame(), ctx, sceneData, _drawExtent);
		}

		end_rendering(cmd);

		blit_to_swapchain(cmd);
		// render gui
		if (guiDrawFunction) {
			VkImageView swapchainImageView = _swapchain.get_image_views()[_swapchainImageIndex];
			guiDrawFunction(cmd, swapchainImageView, _drawExtent);
		}
		end_frame();
	}
}


VkCommandBuffer Renderer::begin_frame()
{
	assert(!_isFrameStarted && "cant call begin_frame while frame is already in progress");


	// wait until the gpu has finished rendering the last frame
	// timeout of 1 second
	FrameData& currentFrame = get_current_frame();
	VK_CHECK(vkWaitForFences(_vulkanCore->get_device(), 1, &currentFrame._renderFence, true, 1000000000));
	currentFrame._deletionQueue.flush();
	currentFrame._frameDescriptors.clear_pools(_vulkanCore->get_device());

	// request image from swapchain
	VkResult acquireResult = vkAcquireNextImageKHR(_vulkanCore->get_device(), _swapchain.get(), 1000000000, currentFrame._swapchainSemaphore, nullptr, &_swapchainImageIndex);

	if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
		recreate_swapchain();
		return VK_NULL_HANDLE;
	}
	if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
		VK_CHECK(acquireResult);
	}

	VK_CHECK(vkResetFences(_vulkanCore->get_device(), 1, &currentFrame._renderFence));

	_isFrameStarted = true;

	VkExtent2D swapchainExtent = _swapchain.get_extent();
	_drawExtent.height = std::min(swapchainExtent.height, _drawImage.imageExtent.height) * renderScale;
	_drawExtent.width = std::min(swapchainExtent.width, _drawImage.imageExtent.width) * renderScale;

	// begin rendering commands
	VkCommandBuffer cmd = currentFrame._mainCommandBuffer;

	// now that we are sure the commands finished executing, we can safely reset the command buffer to begin recording again
	VK_CHECK(vkResetCommandBuffer(cmd, 0));

	// begin command buffer recording
	// we will use this command buffer exactly once, so we want to let vulkan know that
	VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

	// start the command buffer recording
	VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

	return cmd;
}

void Renderer::end_frame()
{
	assert(_isFrameStarted && "cant call end_frame while frame is not in progress");

	FrameData& currentFrame = get_current_frame();
	VkCommandBuffer cmd = currentFrame._mainCommandBuffer;

	// set swapchain image layout to present so we can draw it
	VkImage currentSwapchainImage = _swapchain.get_images()[_swapchainImageIndex];
	vkutil::transition_image(cmd, currentSwapchainImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

	// finalize the command buffer
	// can no longer add commands, but can now be executed
	VK_CHECK(vkEndCommandBuffer(cmd));

	VkSwapchainKHR swapchainHandle = _swapchain.get();
	VkSemaphore renderSemaphore = _swapchain.get_render_semaphores()[_swapchainImageIndex];\

	// prepare the submission to the queue
	// want to wait on the _presentSemaphore, as that semaphore is signaled when the swapchain is ready
	// will signal the _renderSemaphore, to signal that rendering has finished
	VkCommandBufferSubmitInfo cmdInfo = vkinit::command_buffer_submit_info(cmd);
	VkSemaphoreSubmitInfo waitInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR, currentFrame._swapchainSemaphore);
	VkSemaphoreSubmitInfo signalInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, renderSemaphore);

	// submit command buffer to the queue and execute it
	// _renderFence will now block until the graphics commands finish execution
	VkSubmitInfo2 submit = vkinit::submit_info(&cmdInfo, &signalInfo, &waitInfo);
	VK_CHECK(vkQueueSubmit2(_vulkanCore->get_graphics_queue(), 1, &submit, currentFrame._renderFence));


	// prepare present
	// this will put the image we just rendered to the visibile window
	// want to wait on the _renderSemaphore for that, as its necessary that drawing commnads have finished before the image is displayed to the user
	VkPresentInfoKHR presentInfo{
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.pNext = nullptr,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &renderSemaphore,

		.swapchainCount = 1,
		.pSwapchains = &swapchainHandle,

		.pImageIndices = &_swapchainImageIndex
	};

	_isFrameStarted = false;
	_frameNumber++;

	VkResult presentResult = vkQueuePresentKHR(_vulkanCore->get_graphics_queue(), &presentInfo);
	if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR || _window->is_resized()) {
		_window->reset_resized_flag();
		recreate_swapchain();
	}
	else {
		VK_CHECK(presentResult);
	}

}

void Renderer::begin_rendering(VkCommandBuffer cmd)
{
	assert(_isFrameStarted && "cant call begin_rendering while frame is not in progress");

	// begin the render pass
	vkutil::transition_image(cmd, _drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	vkutil::transition_image(cmd, _depthImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

	VkClearValue clearColor{ 0.1f, 0.1f, 0.1f, 1.0f };
	//VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(_drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(_drawImage.imageView, &clearColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(_depthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
	VkRenderingInfo renderInfo = vkinit::rendering_info(_drawExtent, &colorAttachment, &depthAttachment);

	vkCmdBeginRendering(cmd, &renderInfo);
}

void Renderer::end_rendering(VkCommandBuffer cmd)
{
	assert(_isFrameStarted && "cant call end_rendering while frame is not in progress");
	vkCmdEndRendering(cmd);
}

void Renderer::blit_to_swapchain(VkCommandBuffer cmd)
{
	VkImage swapchainImage = _swapchain.get_images()[_swapchainImageIndex];

	// transition the draw image and the swapchain image into their correct transfer layouts
	// so we can blit from the draw image to the swapchain image
	vkutil::transition_image(cmd, _drawImage.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	vkutil::transition_image(cmd, swapchainImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	// copy from draw image into the swapchain
	vkutil::copy_image_to_image(cmd, _drawImage.image, swapchainImage, _drawExtent, _swapchain.get_extent());

	// set swapchain image layout to color attachment optimal for imgui 
	vkutil::transition_image(cmd, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

void Renderer::extract_scene_data(const entt::registry& registry, DrawContext& ctx)
{
	ctx.clear();

	auto view = registry.view<const MeshComponent>();
	for (auto [entity, mesh] : view.each()) {
		if (!mesh.visible || !mesh.mesh) {
			continue;
		}

		glm::mat4 worldMatrix(1.0f);
		
		if (const auto* tc = registry.try_get<TransformComponent>(entity)) {
			worldMatrix = tc->get_transform();
		}

		for (const auto& topNode : mesh.mesh->topNodes) {
			collect_draw_nodes(topNode, worldMatrix, ctx);
		}
	}
}

void Renderer::collect_draw_nodes(const std::shared_ptr<Node>& node, const glm::mat4& parentMatrix, DrawContext& ctx)
{
	glm::mat4 nodeMatrix = parentMatrix * node->localTransform;

	auto meshNode = std::dynamic_pointer_cast<MeshNode>(node);

	if (meshNode && meshNode->mesh) {
		// collect all surfaces of the mesh
		// mesh can have multiple surfaces, each with its own material
		for (auto& surface : meshNode->mesh->surfaces) {
			RenderObject def;
			def.indexCount = surface.count;
			def.firstIndex = surface.startIndex;
			def.indexBuffer = meshNode->mesh->meshBuffers.indexBuffer.buffer;
			def.material = &surface.material->data;
			def.bounds = surface.bounds;
			def.transform = nodeMatrix;
			def.vertexBufferAddress = meshNode->mesh->meshBuffers.vertexBufferAddress;

			if (surface.material->data.passType == MaterialPass::Transparent) {
				ctx.transparentObjects.push_back(def);
			}
			else {
				ctx.opaqueObjects.push_back(def);
			}
		}
	}

	// recurse down
	for (auto& child : node->children) {
		collect_draw_nodes(child, nodeMatrix, ctx);
	}
}

void Renderer::recreate_swapchain()
{
	// wait until the window is not minimized
	VkExtent2D extent = _window->get_extent();
	while (extent.width == 0 || extent.height == 0) {
		extent = _window->get_extent();
		_window->poll_events();
	}

	vkDeviceWaitIdle(_vulkanCore->get_device());

	_swapchain.destroy_swapchain();
	_swapchain.init(_vulkanCore->get_physical_device(), _vulkanCore->get_device(), _vulkanCore->get_surface(), extent.width, extent.height);

	destroy_render_targets();
	create_render_targets();
}

const std::vector<PassStats> Renderer::get_stats() const
{
	std::vector<PassStats> stats;
	for (auto& pass : _computePasses) {
		stats.push_back(pass->get_stats());
	}

	for (auto& pass : _renderPasses) {
		stats.push_back(pass->get_stats());
	}

	return stats;
}


void Renderer::create_command_buffers()
{
	// create command pool for commands submitted to the graphics queue
	// also want the pool to allow for resetting of individual command buffers
	VkCommandPoolCreateInfo commandPoolInfo = vkinit::command_pool_create_info(_vulkanCore->get_graphics_queue_family(), VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);

	std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> frameSizes = {
		{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3},
		{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
		{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3},
		{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}
	};

	for (int i = 0; i < FRAME_OVERLAP; i++) {
		VK_CHECK(vkCreateCommandPool(_vulkanCore->get_device(), &commandPoolInfo, nullptr, &_frames[i]._commandPool));

		// allocate the default command buffer that we will use for rendering
		VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(_frames[i]._commandPool, 1);
		VK_CHECK(vkAllocateCommandBuffers(_vulkanCore->get_device(), &cmdAllocInfo, &_frames[i]._mainCommandBuffer));

		// create a descriptor pool for the frame
		_frames[i]._frameDescriptors.init(_vulkanCore->get_device(), 1000, frameSizes);
	}
}

void Renderer::create_sync_structures()
{
	// create syncronization structures
	// 2 semaphores to syncronize rendering with swapchain
	// want the fence to start signalled so we can wait on it on the first frame
	VkFenceCreateInfo fenceCreateInfo = vkinit::fence_create_info(VK_FENCE_CREATE_SIGNALED_BIT);
	VkSemaphoreCreateInfo semaphoreCreateInfo = vkinit::semaphore_create_info();

	for (int i = 0; i < FRAME_OVERLAP; i++) {
		VK_CHECK(vkCreateFence(_vulkanCore->get_device(), &fenceCreateInfo, nullptr, &_frames[i]._renderFence));
		VK_CHECK(vkCreateSemaphore(_vulkanCore->get_device(), &semaphoreCreateInfo, nullptr, &_frames[i]._swapchainSemaphore));

		_frames[i]._cullStatsBuffer = _vulkanCore->create_buffer(
			sizeof(CullStatsData),
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VMA_MEMORY_USAGE_CPU_TO_GPU
		);
	}
}

void Renderer::create_render_targets()
{
	VkExtent2D windowExtent = _window->get_extent();

	// create a draw image that we will render into, and then blit to the swapchain image

	// draw image size will match the window
	VkExtent3D drawImageExtent = {
		windowExtent.width,
		windowExtent.height,
		1
	};

	// draw image creation
	VkFormat drawImageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
	VkImageUsageFlags drawImageUsages =
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
		VK_IMAGE_USAGE_STORAGE_BIT |
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

	_drawImage = _vulkanCore->create_image(drawImageExtent, drawImageFormat, drawImageUsages, false);

	// create a depth image to use for depth testing

	VkFormat depthImageFormat = VK_FORMAT_D32_SFLOAT;
	VkImageUsageFlags depthImageUsages = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

	_depthImage = _vulkanCore->create_image(drawImageExtent, depthImageFormat, depthImageUsages, false);
}

void Renderer::destroy_render_targets()
{
	if (_drawImage.image != VK_NULL_HANDLE) {
		_vulkanCore->destroy_image(_drawImage);
		_drawImage = {};
	}

	if (_depthImage.image != VK_NULL_HANDLE) {
		_vulkanCore->destroy_image(_depthImage);
		_depthImage = {};
	}
}
