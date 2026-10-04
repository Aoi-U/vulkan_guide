#include "Renderer.h"
#include <utils/vk_initializers.h>
#include <utils/vk_utils.h>
#include <cmath>
#include <algorithm>
#include <resources/vk_loader.h>
#include <core/Components.h>
#include "graphics/GuiRenderer.h"
#include "graphics/RenderGraph.h"

static AutoCVar_Float cvRenderScale("r.renderScale", "Scale multiplier for rendering resolution", 1.0);
AutoCVar_Int cvRenderPath("r.renderPath", "Render Path: 0 = Forward, 1 = Deferred", 1);

AutoCVar_Int cvSSAO("r.ssao", "Enable Screen Space Ambient Occlusion", 1, CVarFlags::EditCheckBox);
AutoCVar_Float cvSSAORadius("r.ssao.radius", "SSAO sample hemisphere radius", 0.5f);
AutoCVar_Float cvSSAOBias("r.ssao.bias", "SSAO depth comparison bias", 0.025f);
AutoCVar_Float cvSSAOPower("r.ssao.power", "SSAO occlusion power/intensity", 1.5f);

void Renderer::init(Window* window, VulkanCore* vulkanCore, MaterialSystem* materialSystem, entt::dispatcher& dispatcher)
{
	_window = window;
	_vulkanCore = vulkanCore;

	_viewportExtent = _window->get_extent();

	_resourceManager.init(_vulkanCore);
	_iblSystem.init(_vulkanCore);

	recreate_swapchain();
	create_command_buffers();
	create_sync_structures();

	dispatcher.sink<Events::WindowResizeEvent>().connect<&Renderer::on_window_resize>(this);

	// create compute passes
	_frustumCullPass = std::make_unique<FrustumCullPass>();
	_frustumCullPass->init(_vulkanCore);

	// create render passes
	_shadowPass = std::make_unique<ShadowPass>();
	_shadowPass->init(_vulkanCore, materialSystem->get_material_layout(), _resourceManager.get_format("SceneColor"), _resourceManager.get_format("SceneDepth"));

	_pointShadowPass = std::make_unique<PointShadowPass>();
	_pointShadowPass->init(_vulkanCore, materialSystem->get_material_layout(), _resourceManager.get_format("SceneColor"), VK_FORMAT_D32_SFLOAT);

	_forwardPass = std::make_unique<ForwardPass>();
	_forwardPass->init(_vulkanCore, materialSystem->get_material_layout(), _resourceManager.get_format("SceneColor"), _resourceManager.get_format("SceneDepth"));	

	_skyboxPass = std::make_unique<SkyboxPass>();
	_skyboxPass->init(_vulkanCore, materialSystem->get_material_layout(), _resourceManager.get_format("SceneColor"), _resourceManager.get_format("SceneDepth"));

	_gbufferPass = std::make_unique<GBufferPass>();
	_gbufferPass->init(_vulkanCore, materialSystem->get_material_layout(),
		_resourceManager.get_format("SceneColor"), _resourceManager.get_format("SceneDepth"));

	_deferredLightingPass = std::make_unique<DeferredLightingPass>();
	_deferredLightingPass->init(_vulkanCore, materialSystem->get_material_layout(), _resourceManager.get_format("SceneColor"), VK_FORMAT_UNDEFINED);

	_ssaoPass = std::make_unique<SSAOPass>();
	_ssaoPass->init(_vulkanCore, nullptr, _resourceManager.get_format("SSAOColor"), VK_FORMAT_UNDEFINED);

	_ssaoBlurPass = std::make_unique<SSAOBlurPass>();
	_ssaoBlurPass->init(_vulkanCore, nullptr, _resourceManager.get_format("SSAOBlur"), VK_FORMAT_UNDEFINED);
}

void Renderer::cleanup(entt::dispatcher& dispatcher)
{
	dispatcher.sink<Events::WindowResizeEvent>().disconnect(this);

	vkDeviceWaitIdle(_vulkanCore->get_device());

	//_meshRenderer->cleanup();
	_frustumCullPass->cleanup();
	_forwardPass->cleanup();
	_pointShadowPass->cleanup();
	_shadowPass->cleanup();
	_skyboxPass->cleanup();
	_gbufferPass->cleanup();
	_deferredLightingPass->cleanup();
	_ssaoPass->cleanup();
	_ssaoBlurPass->cleanup();

	_iblSystem.cleanup();

	_resourceManager.cleanup();

	for (int i = 0; i < FRAME_OVERLAP; i++) {
		_vulkanCore->destroy_buffer(_frames[i]._cullStatsBuffer);

		_vulkanCore->destroy_buffer(_frames[i].sceneDataBuffer);
		_vulkanCore->destroy_buffer(_frames[i]._gpuObjectBuffer);
		_vulkanCore->destroy_buffer(_frames[i]._gpuInstanceBuffer);
		_vulkanCore->destroy_buffer(_frames[i]._gpuIndirectBuffer);
		_vulkanCore->destroy_buffer(_frames[i]._gpuCountBuffer);


		_frames[i]._deletionQueue.flush();
		_frames[i]._frameDescriptors.destroy_pools(_vulkanCore->get_device());

		vkDestroyCommandPool(_vulkanCore->get_device(), _frames[i]._commandPool, nullptr);
		vkDestroyFence(_vulkanCore->get_device(), _frames[i]._renderFence, nullptr);
		vkDestroySemaphore(_vulkanCore->get_device(), _frames[i]._swapchainSemaphore, nullptr);
	}

	_swapchain.cleanup(_vulkanCore->get_device());
}

void Renderer::on_window_resize(const Events::WindowResizeEvent& event)
{
	_resizeRequested = true;
	_pendingResizeExtent = { event.width, event.height };
}

void Renderer::set_viewport_extent(VkExtent2D extent)
{
	if (extent.width == 0 || extent.height == 0)
		return;

	if (_viewportExtent.width == extent.width && _viewportExtent.height == extent.height)
		return;

	vkDeviceWaitIdle(_vulkanCore->get_device());

	// resize swapchain and all window relative render targets
	_viewportExtent = extent;
	_resourceManager.on_window_resize(_viewportExtent);
}

void Renderer::render_frame(const entt::registry& registry, const GPUSceneData& sceneData)
{
	DrawContext drawCtx;
	extract_scene_data(registry, drawCtx);
	drawCtx.environment = _iblSystem.get_environment_resources();

	if (VkCommandBuffer cmd = begin_frame()) {
		RenderContext ctx(cmd);
		FrameData& currentFrame = get_current_frame();

		// upload scene data 
		_vulkanCore->check_and_resize_buffer(currentFrame.sceneDataBuffer, sizeof(GPUSceneData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
		*static_cast<GPUSceneData*>(currentFrame.sceneDataBuffer.info.pMappedData) = sceneData;

		_renderPath = static_cast<RenderPath>(cvRenderPath.Get()); // set render path to forward or deferred from cvar 

		RenderGraph graph(_resourceManager);

		graph.add_pass("FrustumCull")
			.set_execute_callback([&](RenderContext& ctx) {
			_frustumCullPass->dispatch(ctx, currentFrame, drawCtx, sceneData, _drawExtent);
				});

		graph.add_pass("ShadowPass")
			.set_depth_attachment("ShadowMap", VK_ATTACHMENT_LOAD_OP_CLEAR, 1.0f)
			.set_execute_callback([&](RenderContext& ctx) {
			_shadowPass->render(ctx, currentFrame, _resourceManager, drawCtx, sceneData);
		});

		graph.add_pass("PointShadowPass")
			.write("PointShadowMap", ResourceState::DepthAttachment)
			.set_execute_callback([&](RenderContext& ctx) {
			_pointShadowPass->render(ctx, currentFrame, _resourceManager, drawCtx, sceneData);
				});

		if (_renderPath == RenderPath::Deferred) {
			// gbuffer geometry pass
			graph.add_pass("GBufferPass")
				.set_color_attachment("GBufferPosition", VK_ATTACHMENT_LOAD_OP_CLEAR, { 0.0f, 0.0f, 0.0f, 0.0f })
				.set_color_attachment("GBufferNormal", VK_ATTACHMENT_LOAD_OP_CLEAR, { 0.0f, 0.0f, 0.0f, 0.0f })
				.set_color_attachment("GBufferAlbedo", VK_ATTACHMENT_LOAD_OP_CLEAR, { 0.0f, 0.0f, 0.0f, 0.0f })
				.set_color_attachment("GBufferMetalRough", VK_ATTACHMENT_LOAD_OP_CLEAR, { 0.0f, 0.0f, 0.0f, 0.0f })
				.set_depth_attachment("SceneDepth", VK_ATTACHMENT_LOAD_OP_CLEAR, 0.0f)
				.set_execute_callback([&](RenderContext& ctx) {
				_gbufferPass->render(ctx, currentFrame, _resourceManager, drawCtx, sceneData);
					});

			// SSAO generation pass
			graph.add_pass("SSAOPass")
				.read("GBufferPosition", ResourceState::ShaderReadOnly)
				.read("GBufferNormal", ResourceState::ShaderReadOnly)
				.set_color_attachment("SSAOColor", VK_ATTACHMENT_LOAD_OP_CLEAR, { 1.0f, 1.0f, 1.0f, 1.0f })
				.set_execute_callback([&](RenderContext& ctx) {
				_ssaoPass->render(ctx, currentFrame, _resourceManager, drawCtx, sceneData);
					});

			// SSAO blue pass
			graph.add_pass("SSAOBlurPass")
				.read("SSAOColor", ResourceState::ShaderReadOnly)
				.set_color_attachment("SSAOBlur", VK_ATTACHMENT_LOAD_OP_CLEAR, { 1.0f, 1.0f, 1.0f, 1.0f })
				.set_execute_callback([&](RenderContext& ctx) {
				_ssaoBlurPass->render(ctx, currentFrame, _resourceManager, drawCtx, sceneData);
					});

			// deferred lighting pass
			graph.add_pass("DeferredLightingPass")
				.read("GBufferPosition", ResourceState::ShaderReadOnly)
				.read("GBufferNormal", ResourceState::ShaderReadOnly)
				.read("GBufferAlbedo", ResourceState::ShaderReadOnly)
				.read("GBufferMetalRough", ResourceState::ShaderReadOnly)
				.read("ShadowMap", ResourceState::ShaderReadOnly)
				.read("PointShadowMap", ResourceState::ShaderReadOnly)
				.read("SSAOBlur", ResourceState::ShaderReadOnly)
				.set_color_attachment("SceneColor", VK_ATTACHMENT_LOAD_OP_CLEAR, { 0.0f, 0.0f, 0.0f, 1.0f })
				.set_execute_callback([&](RenderContext& ctx) {
				_deferredLightingPass->render(ctx, currentFrame, _resourceManager, drawCtx, sceneData);
					});
		}
		else {
			// forward rendering pass
			graph.add_pass("ForwardPass")
				.read("ShadowMap", ResourceState::ShaderReadOnly)
				.read("PointShadowMap", ResourceState::ShaderReadOnly)
				.read("SkyCubemap", ResourceState::ShaderReadOnly)
				.set_color_attachment("SceneColor", VK_ATTACHMENT_LOAD_OP_CLEAR, { 0.0f, 0.0f, 0.0f, 1.0f })
				.set_depth_attachment("SceneDepth", VK_ATTACHMENT_LOAD_OP_CLEAR, 0.0f)
				.set_execute_callback([&](RenderContext& ctx) {
				_forwardPass->render(ctx, currentFrame, _resourceManager, drawCtx, sceneData);
					});
		}

		// skybox pass
		graph.add_pass("SkyboxPass")
			.read("SkyCubemap", ResourceState::ShaderReadOnly)
			.set_color_attachment("SceneColor", VK_ATTACHMENT_LOAD_OP_LOAD)
			.set_depth_attachment("SceneDepth", VK_ATTACHMENT_LOAD_OP_LOAD)
			.set_execute_callback([&](RenderContext& ctx) {
			_skyboxPass->render(ctx, currentFrame, _resourceManager, drawCtx, sceneData);
				});

		graph.compile();
		graph.execute(ctx);

		// transition scene color to shaderreadonly so imgui can sample it in the viewport window
		_resourceManager.transition(cmd, "SceneColor", ResourceState::ShaderReadOnly);

		// transition swapchain image to color attachment for imgui rendering
		VkImage swapchainImage = _swapchain.get_images()[_swapchainImageIndex];
		RenderSync::transition_image(cmd, swapchainImage, ResourceState::Undefined, ResourceState::ColorAttachment);
		
		if (_guiRenderer) {
			VkImageView swapchainImageView = _swapchain.get_image_views()[_swapchainImageIndex];
			_guiRenderer->render(cmd, swapchainImageView, _swapchain.get_extent());
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

	// begin rendering commands
	VkCommandBuffer cmd = currentFrame._mainCommandBuffer;

	// now that we are sure the commands finished executing, we can safely reset the command buffer to begin recording again
	VK_CHECK(vkResetCommandBuffer(cmd, 0));

	VkExtent2D sceneColorExtent = _resourceManager.get_extent("SceneColor");
	float scale = std::clamp(static_cast<float>(cvRenderScale.Get()), 0.1f, 1.0f);

	_drawExtent.width = std::max(1u, static_cast<uint32_t>(sceneColorExtent.width * scale));
	_drawExtent.height = std::max(1u, static_cast<uint32_t>(sceneColorExtent.height * scale));

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
	RenderSync::transition_image(cmd, currentSwapchainImage, ResourceState::ColorAttachment, ResourceState::Present);

	// finalize the command buffer
	// can no longer add commands, but can now be executed
	VK_CHECK(vkEndCommandBuffer(cmd));

	VkSwapchainKHR swapchainHandle = _swapchain.get();
	VkSemaphore renderSemaphore = _swapchain.get_render_semaphores()[_swapchainImageIndex];

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
	if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR || _resizeRequested) {
		_resizeRequested = false;
		recreate_swapchain();
	}
	else {
		VK_CHECK(presentResult);
	}

}

void Renderer::blit_to_swapchain(VkCommandBuffer cmd)
{
	VkImage swapchainImage = _swapchain.get_images()[_swapchainImageIndex];
	AllocatedImage& sceneColor = _resourceManager.get_image("SceneColor");

	// transition the draw image and the swapchain image into their correct transfer layouts
	// so we can blit from the draw image to the swapchain image
	_resourceManager.transition(cmd, "SceneColor", ResourceState::TransferSrc);

	// transition swapchain image to transferdst to copy into it
	RenderSync::transition_image(cmd, swapchainImage, ResourceState::Undefined, ResourceState::TransferDst);

	// copy SceneColor into the swapchain
	vkutil::copy_image_to_image(cmd, sceneColor.image, swapchainImage, _drawExtent, _swapchain.get_extent());

	// set swapchain image layout to color attachment optimal for imgui 
	RenderSync::transition_image(cmd, swapchainImage, ResourceState::TransferDst, ResourceState::ColorAttachment);
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

	if (ctx.opaqueObjects.empty()) {
		return;
	}

	// sort opaque objects 
	std::sort(ctx.opaqueObjects.begin(), ctx.opaqueObjects.end(), [](const RenderObject& a, const RenderObject& b) {
		if (a.material->materialSet != b.material->materialSet) {
			return a.material->materialSet < b.material->materialSet;
		}
		if (a.indexBuffer != b.indexBuffer) {
			return a.indexBuffer < b.indexBuffer;
		}
		if (a.firstIndex != b.firstIndex) {
			return a.firstIndex < b.firstIndex;
		}
		return a.indexCount < b.indexCount;;
		});

	IndirectBatch currentBatch;
	currentBatch.first = 0;
	currentBatch.count = 0;
	currentBatch.batchIndex = 0;
	currentBatch.material = ctx.opaqueObjects[0].material;
	currentBatch.indexBuffer = ctx.opaqueObjects[0].indexBuffer;

	// compact same objects into multidraw batches
	for (uint32_t i = 0; i < static_cast<uint32_t>(ctx.opaqueObjects.size()); i++) {
		const RenderObject& obj = ctx.opaqueObjects[i];

		bool isSameBatch = (obj.material == currentBatch.material) && (obj.indexBuffer == currentBatch.indexBuffer);

		if (!isSameBatch) {
			ctx.opaqueBatches.push_back(currentBatch);
			currentBatch.first = i;
			currentBatch.count = 0;
			currentBatch.batchIndex = static_cast<uint32_t>(ctx.opaqueBatches.size());
			currentBatch.material = obj.material;
			currentBatch.indexBuffer = obj.indexBuffer;
		}
		currentBatch.count++;
	}
	ctx.opaqueBatches.push_back(currentBatch);
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

			if (surface.material->data.passType == MaterialPassType::Transparent) {
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
	vkDeviceWaitIdle(_vulkanCore->get_device());

	VkExtent2D actualExtent = _pendingResizeExtent;
	if (actualExtent.width == 0 || actualExtent.height == 0) {
		actualExtent = _window->get_extent();
	}

	if (actualExtent.width == 0 || actualExtent.height == 0) {
		return; // window minimized
	}

	_swapchain.destroy_swapchain();
	_swapchain.init(_vulkanCore->get_physical_device(), _vulkanCore->get_device(), _vulkanCore->get_surface(), actualExtent.width, actualExtent.height);

	if (!_resourceManager.has_target("SceneColor")) {
		create_render_targets();
	}
	else {
		_resourceManager.on_window_resize(_pendingResizeExtent);
	}
}

VkDescriptorSet Renderer::get_scene_color_descriptor()
{
	if (!_guiRenderer)
		return VK_NULL_HANDLE;

	VkImageView sceneColorView = _resourceManager.get_view("SceneColor");
	return _guiRenderer->get_image_descriptor(sceneColorView, _vulkanCore->_defaultSamplerLinear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

const std::vector<PassStats> Renderer::get_stats() const
{
	std::vector<PassStats> stats;
	stats.push_back(_frustumCullPass->get_stats());
	stats.push_back(_shadowPass->get_stats());
	stats.push_back(_pointShadowPass->get_stats());

	if (_renderPath == RenderPath::Forward) {
		stats.push_back(_forwardPass->get_stats());
	}
	else {
		stats.push_back(_gbufferPass->get_stats());
		stats.push_back(_ssaoPass->get_stats());
		stats.push_back(_ssaoBlurPass->get_stats());
		stats.push_back(_deferredLightingPass->get_stats());
	}
	stats.push_back(_skyboxPass->get_stats());

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
		{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 10}
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

		_frames[i]._cullStatsBuffer = _vulkanCore->create_buffer(sizeof(CullStatsData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	}
}

void Renderer::create_render_targets()
{
	VkExtent2D windowExtent = _window->get_extent();
	VkExtent2D targetExtent = _viewportExtent;
	if (targetExtent.width == 0 || targetExtent.height == 0) {
		targetExtent = _window->get_extent();
	}

	// create primary scene color 
	_resourceManager.register_target({
		.name = "SceneColor",
		.sizePolicy = TargetSizePolicy::WindowRelative,
		.relativeScale = 1.0f,
		.format = VK_FORMAT_R16G16B16A16_SFLOAT,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
		}, targetExtent);
	
	// create primary scene depth
	_resourceManager.register_target({
		.name = "SceneDepth",
		.sizePolicy = TargetSizePolicy::WindowRelative,
		.relativeScale = 1.0f,
		.format = VK_FORMAT_D32_SFLOAT,
		.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT
		}, targetExtent);

	// create shadow map target
	_resourceManager.register_target({
		.name = "ShadowMap",
		.sizePolicy = TargetSizePolicy::Absolute,
		.absoluteExtent = { 2048, 2048 },
		.format = VK_FORMAT_D32_SFLOAT,
		.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT
		}, targetExtent);

	// create point shadow target
	_resourceManager.register_target({
		.name = "PointShadowMap",
		.sizePolicy = TargetSizePolicy::Absolute,
		.absoluteExtent = { 1024, 1024 },
		.format = VK_FORMAT_D32_SFLOAT,
		.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.mipLevels = 1,
		.arrayLayers = 6,
		.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
		.viewType = VK_IMAGE_VIEW_TYPE_CUBE,
		.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT
		}, targetExtent);

	// create skybox target
	_resourceManager.register_target({
		.name = "SkyCubemap",
		.sizePolicy = TargetSizePolicy::Absolute,
		.absoluteExtent = { 256, 256 },
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.mipLevels = 1,
		.arrayLayers = 6,
		.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
		.viewType = VK_IMAGE_VIEW_TYPE_CUBE,
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
		}, targetExtent);

	// create gbuffer position target
	_resourceManager.register_target({
		.name = "GBufferPosition",
		.sizePolicy = TargetSizePolicy::WindowRelative,
		.relativeScale = 1.0f,
		.format = VK_FORMAT_R16G16B16A16_SFLOAT,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
		}, targetExtent);

	// create gbuffer normal target
	_resourceManager.register_target({
			.name = "GBufferNormal",
			.sizePolicy = TargetSizePolicy::WindowRelative,
			.relativeScale = 1.0f,
			.format = VK_FORMAT_R16G16B16A16_SFLOAT,
			.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
		}, targetExtent);

	// create gbuffer albedo target
	_resourceManager.register_target({
			.name = "GBufferAlbedo",
			.sizePolicy = TargetSizePolicy::WindowRelative,
			.relativeScale = 1.0f,
			.format = VK_FORMAT_R8G8B8A8_UNORM,
			.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
		}, targetExtent);

	// create gbuffer metallic/roughness target
	_resourceManager.register_target({
			.name = "GBufferMetalRough",
			.sizePolicy = TargetSizePolicy::WindowRelative,
			.relativeScale = 1.0f,
			.format = VK_FORMAT_R8G8B8A8_UNORM,
			.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
		}, targetExtent);

	// create ssao color target
	_resourceManager.register_target({
				.name = "SSAOColor",
				.sizePolicy = TargetSizePolicy::WindowRelative,
				.relativeScale = 1.0f,
				.format = VK_FORMAT_R8_UNORM,
				.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
		}, targetExtent);

	// create ssao blur target
	_resourceManager.register_target({
			.name = "SSAOBlur",
			.sizePolicy = TargetSizePolicy::WindowRelative,
			.relativeScale = 1.0f,
			.format = VK_FORMAT_R8_UNORM,
			.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
		}, targetExtent);
}
