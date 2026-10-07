#include "Renderer.h"
#include <utils/vk_initializers.h>
#include <utils/vk_utils.h>
#include <cmath>
#include <algorithm>
#include <resources/Model.h>
#include <core/Components.h>
#include "graphics/GuiRenderer.h"
#include "graphics/RenderGraph.h"

AutoCVar_Float cvRenderScale("r.renderScale", "Scale multiplier for rendering resolution", 1.0, 0.1, 1.0, CVarFlags::EditFloatSlider);

AutoCVar_Int cvSSAO("r.ssao", "Enable Screen Space Ambient Occlusion", 1, CVarFlags::EditCheckBox);
AutoCVar_Float cvSSAORadius("r.ssao.radius", "SSAO sample hemisphere radius", 0.5f, 0.05, 3.0, CVarFlags::EditFloatSlider);
AutoCVar_Float cvSSAOBias("r.ssao.bias", "SSAO depth comparison bias", 0.025f, 0.001, 0.1, CVarFlags::EditFloatSlider);
AutoCVar_Float cvSSAOPower("r.ssao.power", "SSAO occlusion power/intensity", 1.5f, 0.1, 5.0, CVarFlags::EditFloatSlider);

void Renderer::init(Window* window, VulkanCore* vulkanCore, entt::dispatcher& dispatcher, ResourceManager* resourceManager)
{
	_window = window;
	_vulkanCore = vulkanCore;

	_viewportExtent = _window->get_extent();

	_renderResourceManager.init(_vulkanCore);
	_iblSystem.init(_vulkanCore, resourceManager);

	recreate_swapchain();
	create_command_buffers();
	create_sync_structures();

	dispatcher.sink<Events::WindowResizeEvent>().connect<&Renderer::on_window_resize>(this);

	// create compute passes
	_frustumCullPass = std::make_unique<FrustumCullPass>();
	_frustumCullPass->init(_vulkanCore, resourceManager);

	VkDescriptorSetLayout matLayout = Material::get_descriptor_layout();

	// create render passes
	_shadowPass = std::make_unique<ShadowPass>();
	_shadowPass->init(_vulkanCore, resourceManager, matLayout, _renderResourceManager.get_format("SceneColor"), _renderResourceManager.get_format("SceneDepth"));

	_pointShadowPass = std::make_unique<PointShadowPass>();
	_pointShadowPass->init(_vulkanCore, resourceManager, matLayout, _renderResourceManager.get_format("SceneColor"), VK_FORMAT_D32_SFLOAT);

	_skyboxPass = std::make_unique<SkyboxPass>();
	_skyboxPass->init(_vulkanCore, resourceManager, matLayout, _renderResourceManager.get_format("SceneColor"), _renderResourceManager.get_format("SceneDepth"));

	_gbufferPass = std::make_unique<GBufferPass>();
	_gbufferPass->init(_vulkanCore, resourceManager, matLayout, _renderResourceManager.get_format("SceneColor"), _renderResourceManager.get_format("SceneDepth"));

	_deferredLightingPass = std::make_unique<DeferredLightingPass>();
	_deferredLightingPass->init(_vulkanCore, resourceManager, matLayout, _renderResourceManager.get_format("SceneColor"), VK_FORMAT_UNDEFINED);

	_ssaoPass = std::make_unique<SSAOPass>();
	_ssaoPass->init(_vulkanCore, resourceManager, nullptr, _renderResourceManager.get_format("SSAOColor"), VK_FORMAT_UNDEFINED);

	_ssaoBlurPass = std::make_unique<SSAOBlurPass>();
	_ssaoBlurPass->init(_vulkanCore, resourceManager, nullptr, _renderResourceManager.get_format("SSAOBlur"), VK_FORMAT_UNDEFINED);
}

void Renderer::cleanup(entt::dispatcher& dispatcher)
{
	dispatcher.sink<Events::WindowResizeEvent>().disconnect(this);

	vkDeviceWaitIdle(_vulkanCore->get_device());

	_frustumCullPass->cleanup();
	_pointShadowPass->cleanup();
	_shadowPass->cleanup();
	_skyboxPass->cleanup();
	_gbufferPass->cleanup();
	_deferredLightingPass->cleanup();
	_ssaoPass->cleanup();
	_ssaoBlurPass->cleanup();
	_iblSystem.cleanup();
	_renderResourceManager.cleanup();

	for (int i = 0; i < FRAME_OVERLAP; i++) {
		_frames[i]._cullStatsBuffer.unload(_vulkanCore);
		_frames[i].sceneDataBuffer.unload(_vulkanCore);
		_frames[i]._gpuObjectBuffer.unload(_vulkanCore);
		_frames[i]._gpuInstanceBuffer.unload(_vulkanCore);
		_frames[i]._gpuIndirectBuffer.unload(_vulkanCore);
		_frames[i]._gpuCountBuffer.unload(_vulkanCore);
		_frames[i]._materialDataBuffer.unload(_vulkanCore);


		_frames[i]._deletionQueue.flush(_vulkanCore->get_device(), _vulkanCore->get_allocator());
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

	float scale = std::clamp(static_cast<float>(cvRenderScale.Get()), 0.1f, 1.0f);
	bool extentChanged = (_viewportExtent.width != extent.width || _viewportExtent.height != extent.height);
	bool scaleChanged = (std::abs(scale - _currentRenderScale) > 0.001f);

	if (!extentChanged && !scaleChanged) {
		return;
	}

	// wait for GPU to finish before destroying images and freeing descriptor sets
	vkDeviceWaitIdle(_vulkanCore->get_device());

	// clear cached image descriptors in gui renderer so they will be recreated with new viewport extent
	if (_guiRenderer) {
		_guiRenderer->clear_image_descriptors();
	}

	_viewportExtent = extent;
	_currentRenderScale = scale;
	_drawExtent.width = std::max(1u, static_cast<uint32_t>(_viewportExtent.width * _currentRenderScale));
	_drawExtent.height = std::max(1u, static_cast<uint32_t>(_viewportExtent.height * _currentRenderScale));

	_renderResourceManager.on_window_resize(_drawExtent);

	// update the descriptor 
	if (_guiRenderer) {
		VkImageView newView = _renderResourceManager.get_view("SceneColor");
		_guiRenderer->get_image_descriptor(newView, _vulkanCore->_defaultSamplerLinear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
}

void Renderer::render_frame(const entt::registry& registry, const GPUSceneData& sceneData)
{

	if (VkCommandBuffer cmd = begin_frame()) {
		DrawContext drawCtx;
		drawCtx.environment = _iblSystem.get_environment_resources();
		extract_scene_data(registry, drawCtx);

		RenderContext ctx(cmd);
		FrameData& currentFrame = get_current_frame();

		// upload scene data 
		currentFrame.sceneDataBuffer.resize(_vulkanCore, sizeof(GPUSceneData));
		currentFrame.sceneDataBuffer.copy_data(&sceneData, sizeof(GPUSceneData));

		RenderGraph graph(_renderResourceManager);

		graph.add_pass("FrustumCull")
			.set_execute_callback([&](RenderContext& ctx) {
			_frustumCullPass->dispatch(ctx, currentFrame, drawCtx, sceneData, _drawExtent);
				});

		graph.add_pass("ShadowPass")
			.set_depth_attachment("ShadowMap", VK_ATTACHMENT_LOAD_OP_CLEAR, 1.0f)
			.set_execute_callback([&](RenderContext& ctx) {
			_shadowPass->render(ctx, currentFrame, _renderResourceManager, drawCtx, sceneData);
		});

		graph.add_pass("PointShadowPass")
			.write("PointShadowMap", ResourceState::DepthAttachment)
			.set_execute_callback([&](RenderContext& ctx) {
			_pointShadowPass->render(ctx, currentFrame, _renderResourceManager, drawCtx, sceneData);
				});

		// gbuffer geometry pass
		graph.add_pass("GBufferPass")
			.set_color_attachment("GBufferPosition")
			.set_color_attachment("GBufferNormal")
			.set_color_attachment("GBufferAlbedo")
			.set_color_attachment("GBufferMetalRough")
			.set_color_attachment("GBufferEmissive")
			.set_depth_attachment("SceneDepth", VK_ATTACHMENT_LOAD_OP_CLEAR, 0.0f)
			.set_execute_callback([&](RenderContext& ctx) {
			_gbufferPass->render(ctx, currentFrame, _renderResourceManager, drawCtx, sceneData);
				});

		// SSAO generation pass
		graph.add_pass("SSAOPass")
			.read("GBufferPosition", ResourceState::ShaderReadOnly)
			.read("GBufferNormal", ResourceState::ShaderReadOnly)
			.set_color_attachment("SSAOColor", VK_ATTACHMENT_LOAD_OP_CLEAR, { 1.0f, 1.0f, 1.0f, 1.0f })
			.set_execute_callback([&](RenderContext& ctx) {
			_ssaoPass->render(ctx, currentFrame, _renderResourceManager, drawCtx, sceneData);
				});

		// SSAO blue pass
		graph.add_pass("SSAOBlurPass")
			.read("SSAOColor", ResourceState::ShaderReadOnly)
			.set_color_attachment("SSAOBlur", VK_ATTACHMENT_LOAD_OP_CLEAR, { 1.0f, 1.0f, 1.0f, 1.0f })
			.set_execute_callback([&](RenderContext& ctx) {
			_ssaoBlurPass->render(ctx, currentFrame, _renderResourceManager, drawCtx, sceneData);
				});

		// deferred lighting pass
		graph.add_pass("DeferredLightingPass")
			.read("GBufferPosition", ResourceState::ShaderReadOnly)
			.read("GBufferNormal", ResourceState::ShaderReadOnly)
			.read("GBufferAlbedo", ResourceState::ShaderReadOnly)
			.read("GBufferMetalRough", ResourceState::ShaderReadOnly)
			.read("GBufferEmissive", ResourceState::ShaderReadOnly)
			.read("ShadowMap", ResourceState::ShaderReadOnly)
			.read("PointShadowMap", ResourceState::ShaderReadOnly)
			.read("SSAOBlur", ResourceState::ShaderReadOnly)
			.set_color_attachment("SceneColor", VK_ATTACHMENT_LOAD_OP_CLEAR, { 0.0f, 0.0f, 0.0f, 1.0f })
			.set_execute_callback([&](RenderContext& ctx) {
			_deferredLightingPass->render(ctx, currentFrame, _renderResourceManager, drawCtx, sceneData);
				});

		// skybox pass
		graph.add_pass("SkyboxPass")
			.read("SkyCubemap", ResourceState::ShaderReadOnly)
			.set_color_attachment("SceneColor", VK_ATTACHMENT_LOAD_OP_LOAD)
			.set_depth_attachment("SceneDepth", VK_ATTACHMENT_LOAD_OP_LOAD)
			.set_execute_callback([&](RenderContext& ctx) {
			_skyboxPass->render(ctx, currentFrame, _renderResourceManager, drawCtx, sceneData);
				});

		graph.compile();
		graph.execute(ctx);

		// transition scene color to shaderreadonly so imgui can sample it in the viewport window
		//_resourceManager.transition(cmd, "SceneColor", ResourceState::ShaderReadOnly);

		// transition al render targets to shaderreadonly for viewing in imgui
		_renderResourceManager.transition_all_to_read_only(cmd);

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
	currentFrame._deletionQueue.flush(_vulkanCore->get_device(), _vulkanCore->get_allocator());
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

	VkExtent2D sceneColorExtent = _renderResourceManager.get_extent("SceneColor");
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
	Texture& sceneColor = _renderResourceManager.get_image("SceneColor");

	// transition the draw image and the swapchain image into their correct transfer layouts
	// so we can blit from the draw image to the swapchain image
	_renderResourceManager.transition(cmd, "SceneColor", ResourceState::TransferSrc);

	// transition swapchain image to transferdst to copy into it
	RenderSync::transition_image(cmd, swapchainImage, ResourceState::Undefined, ResourceState::TransferDst);

	// copy SceneColor into the swapchain
	vkutil::copy_image_to_image(cmd, sceneColor.get_image(), swapchainImage, _drawExtent, _swapchain.get_extent());

	// set swapchain image layout to color attachment optimal for imgui 
	RenderSync::transition_image(cmd, swapchainImage, ResourceState::TransferDst, ResourceState::ColorAttachment);
}

void Renderer::extract_scene_data(const entt::registry& registry, DrawContext& ctx)
{
	auto compute_world_transform = [&](entt::entity entity) -> glm::mat4 {
		glm::mat4 world(1.0f);
		entt::entity current = entity;
		while (current != entt::null) {
			if (const auto* tc = registry.try_get<TransformComponent>(current)) {
				world = tc->get_transform() * world;
			}
			if (const auto* hc = registry.try_get<HierarchyComponent>(current)) {
				current = hc->parent;
			}
			else {
				current = entt::null;
			}
		}
		return world;
		};

	FrameData& currentFrame = get_current_frame();
	size_t capacity = currentFrame._materialDataBuffer.get_size() / sizeof(MaterialConstants); // material data buffer capacity
	uint32_t materialCount = 0; 
	MaterialConstants* matDataMap = static_cast<MaterialConstants*>(currentFrame._materialDataBuffer.get_mapped_data()); // pointer to the buffers mapped data

	auto view = registry.view<const MeshComponent>();
	for (auto [entity, meshComp] : view.each()) {
		if (!meshComp.visible || !meshComp.mesh.is_valid()) {
			continue;
		}
		glm::mat4 worldMatrix = compute_world_transform(entity);
		auto* matComp = registry.try_get<MaterialComponent>(entity);

		VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
		MaterialPassType passType = MaterialPassType::Opaque;
		bool isDoubleSided = false;

		if (matComp) {
			passType = (matComp->alphaMode == AlphaMode::Blend) ? MaterialPassType::Transparent : MaterialPassType::Opaque;
			isDoubleSided = matComp->doubleSided;

			// expand buffer capacity if needed
			if (materialCount >= capacity) {
				currentFrame._materialDataBuffer.resize(_vulkanCore, (capacity + 1000) * sizeof(MaterialConstants), true);
				capacity = currentFrame._materialDataBuffer.get_size() / sizeof(MaterialConstants);
				matDataMap = static_cast<MaterialConstants*>(currentFrame._materialDataBuffer.get_mapped_data()); // update the pointer to the buffers mapped data
			}

			matDataMap[materialCount] = get_material_constants(*matComp); // copy material constants into the buffer
			size_t bufferOffset = sizeof(MaterialConstants) * materialCount; // offset in the buffer for this material
			materialCount++; // increment material count for next material

			descriptorSet = currentFrame._frameDescriptors.allocate(_vulkanCore->get_device(), Material::get_descriptor_layout()); 

			DescriptorWriter writer;
			writer.write_buffer(0, currentFrame._materialDataBuffer.get_buffer(), sizeof(MaterialConstants), bufferOffset, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);

			VkImageView albedoView = matComp->albedoTexture.is_valid() ? matComp->albedoTexture->get_image_view() : _vulkanCore->_greyImage.get_image_view();
			VkSampler albedoSampler = matComp->albedoTexture.is_valid() ? matComp->albedoTexture->get_sampler() : _vulkanCore->_defaultSamplerLinear;

			VkImageView metalRoughView = matComp->metallicRoughnessTexture.is_valid() ? matComp->metallicRoughnessTexture->get_image_view() : _vulkanCore->_whiteImage.get_image_view();
			VkSampler metalRoughSampler = matComp->metallicRoughnessTexture.is_valid() ? matComp->metallicRoughnessTexture->get_sampler() : _vulkanCore->_defaultSamplerLinear;

			VkImageView occlusionView = matComp->occlusionTexture.is_valid() ? matComp->occlusionTexture->get_image_view() : _vulkanCore->_whiteImage.get_image_view();
			VkSampler occlusionSampler = matComp->occlusionTexture.is_valid() ? matComp->occlusionTexture->get_sampler() : _vulkanCore->_defaultSamplerLinear;

			VkImageView normalView = matComp->normalTexture.is_valid() ? matComp->normalTexture->get_image_view() : _vulkanCore->_defaultNormalImage.get_image_view();
			VkSampler normalSampler = matComp->normalTexture.is_valid() ? matComp->normalTexture->get_sampler() : _vulkanCore->_defaultSamplerLinear;

			VkImageView emissiveView = matComp->emissiveTexture.is_valid() ? matComp->emissiveTexture->get_image_view() : _vulkanCore->_blackImage.get_image_view();
			VkSampler emissiveSampler = matComp->emissiveTexture.is_valid() ? matComp->emissiveTexture->get_sampler() : _vulkanCore->_defaultSamplerLinear;

			writer.write_image(1, albedoView, albedoSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			writer.write_image(2, metalRoughView, metalRoughSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			writer.write_image(3, occlusionView, occlusionSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			writer.write_image(4, normalView, normalSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			writer.write_image(5, emissiveView, emissiveSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

			writer.update_set(_vulkanCore->get_device(), descriptorSet); // update descriptor set for this material
			matComp->isDirty = false;
		}
		else { // no material component for this entity
			// fallback material
			static Buffer fallbackBuffer;
			if (!fallbackBuffer.get_buffer()) {
				MaterialConstants defaultConstants{};
				defaultConstants.colorFactors = glm::vec4(1.0f);
				defaultConstants.metalRoughFactors = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
				defaultConstants.extraFactors = glm::vec4(1.0f, 1.0f, 0.0f, 0.5f);
				defaultConstants.emissiveFactor = glm::vec4(0.0f);
				fallbackBuffer = Buffer("fallback_mat_buf", sizeof(MaterialConstants), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
				fallbackBuffer.load(_vulkanCore);
				fallbackBuffer.copy_data(&defaultConstants, sizeof(MaterialConstants));
			}

			descriptorSet = currentFrame._frameDescriptors.allocate(_vulkanCore->get_device(), Material::get_descriptor_layout());
			DescriptorWriter writer;
			writer.write_buffer(0, fallbackBuffer.get_buffer(), sizeof(MaterialConstants), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
			writer.write_image(1, _vulkanCore->_greyImage.get_image_view(), _vulkanCore->_defaultSamplerLinear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			writer.write_image(2, _vulkanCore->_whiteImage.get_image_view(), _vulkanCore->_defaultSamplerLinear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			writer.write_image(3, _vulkanCore->_whiteImage.get_image_view(), _vulkanCore->_defaultSamplerLinear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			writer.write_image(4, _vulkanCore->_defaultNormalImage.get_image_view(), _vulkanCore->_defaultSamplerLinear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			writer.write_image(5, _vulkanCore->_blackImage.get_image_view(), _vulkanCore->_defaultSamplerLinear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			writer.update_set(_vulkanCore->get_device(), descriptorSet);
		}

		RenderObject def;
		def.indexCount = meshComp.count;
		def.firstIndex = meshComp.startIndex;
		def.indexBuffer = meshComp.mesh->get_index_buffer();
		def.vertexBufferAddress = meshComp.mesh->get_vertex_buffer_address();
		def.bounds = meshComp.bounds;
		def.transform = worldMatrix;
		def.materialSet = descriptorSet;
		def.passType = passType;
		def.isDoubleSided = isDoubleSided;

		if (def.passType == MaterialPassType::Transparent) {
			ctx.transparentObjects.push_back(def);
		}
		else {
			ctx.opaqueObjects.push_back(def);
		}
	}

	if (ctx.opaqueObjects.empty()) {
		return;
	}

	// sort opaque objects 
	std::sort(ctx.opaqueObjects.begin(), ctx.opaqueObjects.end(), [](const RenderObject& a, const RenderObject& b) {
		if (a.isDoubleSided != b.isDoubleSided) 
			return a.isDoubleSided < b.isDoubleSided;

		if (a.materialSet != b.materialSet) 
			return a.materialSet < b.materialSet;
		
		if (a.indexBuffer != b.indexBuffer) 
			return a.indexBuffer < b.indexBuffer;
		
		if (a.firstIndex != b.firstIndex) 
			return a.firstIndex < b.firstIndex;
		
		return a.indexCount < b.indexCount;;
		});

	IndirectBatch currentBatch;
	currentBatch.first = 0;
	currentBatch.count = 0;
	currentBatch.batchIndex = 0;
	currentBatch.materialSet = ctx.opaqueObjects[0].materialSet;
	currentBatch.indexBuffer = ctx.opaqueObjects[0].indexBuffer;
	currentBatch.isDoubleSided = ctx.opaqueObjects[0].isDoubleSided; 

	// compact same objects into multidraw batches
	for (uint32_t i = 0; i < static_cast<uint32_t>(ctx.opaqueObjects.size()); i++) {
		const RenderObject& obj = ctx.opaqueObjects[i];

		const RenderObject& firstObj = ctx.opaqueObjects[currentBatch.first];

		bool isSameBatch = (obj.materialSet == firstObj.materialSet) &&
			(obj.indexBuffer == firstObj.indexBuffer) &&
			(obj.firstIndex == firstObj.firstIndex) &&
			(obj.indexCount == firstObj.indexCount) &&
			(obj.isDoubleSided == firstObj.isDoubleSided);

		if (!isSameBatch) {
			ctx.opaqueBatches.push_back(currentBatch);
			currentBatch.first = i;
			currentBatch.count = 0;
			currentBatch.batchIndex = static_cast<uint32_t>(ctx.opaqueBatches.size());
			currentBatch.materialSet = obj.materialSet;
			currentBatch.indexBuffer = obj.indexBuffer;
			currentBatch.isDoubleSided = obj.isDoubleSided;
		}
		currentBatch.count++;
	}
	ctx.opaqueBatches.push_back(currentBatch);
}


void Renderer::recreate_swapchain()
{
	vkDeviceWaitIdle(_vulkanCore->get_device());

	// clear cached image descriptors in gui renderer so they will be recreated with new viewport extent
	if (_guiRenderer) {
		_guiRenderer->clear_image_descriptors();
	}

	VkExtent2D actualExtent = _pendingResizeExtent;
	if (actualExtent.width == 0 || actualExtent.height == 0) {
		actualExtent = _window->get_extent();
	}

	if (actualExtent.width == 0 || actualExtent.height == 0) {
		return; // window minimized
	}

	_swapchain.destroy_swapchain();
	_swapchain.init(_vulkanCore->get_physical_device(), _vulkanCore->get_device(), _vulkanCore->get_surface(), actualExtent.width, actualExtent.height);

	if (!_renderResourceManager.has_target("SceneColor")) {
		create_render_targets();
	}
	else {
		_renderResourceManager.on_window_resize(_pendingResizeExtent);
	}

	if (_guiRenderer) {
		VkImageView newView = _renderResourceManager.get_view("SceneColor");
		_guiRenderer->get_image_descriptor(newView, _vulkanCore->_defaultSamplerLinear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
}

VkDescriptorSet Renderer::get_target_descriptor(const std::string& name, uint32_t faceIndex)
{
	if (!_guiRenderer || !_renderResourceManager.has_target(name)) {
		return VK_NULL_HANDLE;
	}

	const auto& desc = _renderResourceManager.get_description(name);
	VkImageView imageView = VK_NULL_HANDLE;

	bool isCube = (desc.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) || (desc.arrayLayers == 6 && desc.viewType == VK_IMAGE_VIEW_TYPE_CUBE);
	if (isCube) {
		if (faceIndex < 6) {
			imageView = _renderResourceManager.get_face_view(name, faceIndex);
		}
	}
	else {
		imageView = _renderResourceManager.get_view(name);
	}

	if (imageView == VK_NULL_HANDLE) {
		return VK_NULL_HANDLE;
	}

	return _guiRenderer->get_image_descriptor(imageView, _vulkanCore->_defaultSamplerLinear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

VkDescriptorSet Renderer::get_scene_color_descriptor()
{
	return get_target_descriptor("SceneColor");
}

const std::vector<PassStats> Renderer::get_stats() const
{
	std::vector<PassStats> stats;
	stats.push_back(_frustumCullPass->get_stats());
	stats.push_back(_shadowPass->get_stats());
	stats.push_back(_pointShadowPass->get_stats());
	stats.push_back(_gbufferPass->get_stats());
	stats.push_back(_ssaoPass->get_stats());
	stats.push_back(_ssaoBlurPass->get_stats());
	stats.push_back(_deferredLightingPass->get_stats());
	
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

		_frames[i]._cullStatsBuffer = Buffer("cullStats", sizeof(CullStatsData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
		_frames[i]._cullStatsBuffer.load(_vulkanCore);

		_frames[i].sceneDataBuffer = Buffer("sceneData", sizeof(GPUSceneData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
		_frames[i].sceneDataBuffer.load(_vulkanCore);

		_frames[i]._gpuObjectBuffer = Buffer("gpuObject", 0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
		_frames[i]._gpuInstanceBuffer = Buffer("gpuInstance", 0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		_frames[i]._gpuIndirectBuffer = Buffer("gpuIndirect", 0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
		_frames[i]._gpuCountBuffer = Buffer("gpuCount", 0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

		_frames[i]._materialDataBuffer = Buffer("materialData", sizeof(MaterialConstants) * 1000, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
		_frames[i]._materialDataBuffer.load(_vulkanCore);
	}
}

void Renderer::create_render_targets()
{
	float scale = std::clamp(static_cast<float>(cvRenderScale.Get()), 0.1f, 1.0f);
	_currentRenderScale = scale;
	_drawExtent.width = std::max(1u, static_cast<uint32_t>(_viewportExtent.width * _currentRenderScale));
	_drawExtent.height = std::max(1u, static_cast<uint32_t>(_viewportExtent.height * _currentRenderScale));

	VkExtent2D targetExtent = _drawExtent;
	if (targetExtent.width == 0 || targetExtent.height == 0) {
		targetExtent = _window->get_extent();
	}

	// create primary scene color 
	_renderResourceManager.register_target({
		.name = "SceneColor",
		.sizePolicy = TargetSizePolicy::WindowRelative,
		.relativeScale = 1.0f,
		.format = VK_FORMAT_R16G16B16A16_SFLOAT,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
		}, targetExtent);
	
	// create primary scene depth
	_renderResourceManager.register_target({
		.name = "SceneDepth",
		.sizePolicy = TargetSizePolicy::WindowRelative,
		.relativeScale = 1.0f,
		.format = VK_FORMAT_D32_SFLOAT,
		.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT
		}, targetExtent);

	// create shadow map target
	_renderResourceManager.register_target({
		.name = "ShadowMap",
		.sizePolicy = TargetSizePolicy::Absolute,
		.absoluteExtent = { 2048, 2048 },
		.format = VK_FORMAT_D32_SFLOAT,
		.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT
		}, targetExtent);

	// create point shadow target
	_renderResourceManager.register_target({
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
	_renderResourceManager.register_target({
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
	_renderResourceManager.register_target({
		.name = "GBufferPosition",
		.sizePolicy = TargetSizePolicy::WindowRelative,
		.relativeScale = 1.0f,
		.format = VK_FORMAT_R16G16B16A16_SFLOAT,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
		}, targetExtent);

	// create gbuffer normal target
	_renderResourceManager.register_target({
			.name = "GBufferNormal",
			.sizePolicy = TargetSizePolicy::WindowRelative,
			.relativeScale = 1.0f,
			.format = VK_FORMAT_R16G16B16A16_SFLOAT,
			.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
		}, targetExtent);

	// create gbuffer albedo target
	_renderResourceManager.register_target({
			.name = "GBufferAlbedo",
			.sizePolicy = TargetSizePolicy::WindowRelative,
			.relativeScale = 1.0f,
			.format = VK_FORMAT_R8G8B8A8_UNORM,
			.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
		}, targetExtent);

	// create gbuffer metallic/roughness target
	_renderResourceManager.register_target({
			.name = "GBufferMetalRough",
			.sizePolicy = TargetSizePolicy::WindowRelative,
			.relativeScale = 1.0f,
			.format = VK_FORMAT_R8G8B8A8_UNORM,
			.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
		}, targetExtent);

	// create gbuffer emissive target
	_renderResourceManager.register_target({
		.name = "GBufferEmissive",
		.sizePolicy = TargetSizePolicy::WindowRelative,
		.relativeScale = 1.0f,
		.format = VK_FORMAT_R16G16B16A16_SFLOAT,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
		}, targetExtent);

	// create ssao color target
	_renderResourceManager.register_target({
				.name = "SSAOColor",
				.sizePolicy = TargetSizePolicy::WindowRelative,
				.relativeScale = 1.0f,
				.format = VK_FORMAT_R8_UNORM,
				.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
		}, targetExtent);

	// create ssao blur target
	_renderResourceManager.register_target({
			.name = "SSAOBlur",
			.sizePolicy = TargetSizePolicy::WindowRelative,
			.relativeScale = 1.0f,
			.format = VK_FORMAT_R8_UNORM,
			.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
		}, targetExtent);
	
}

void Renderer::update_render_extent()
{
	float scale = std::clamp(static_cast<float>(cvRenderScale.Get()), 0.1f, 1.0f);
	_currentRenderScale = scale;
	_drawExtent.width = std::max(1u, static_cast<uint32_t>(_viewportExtent.width * _currentRenderScale));
	_drawExtent.height = std::max(1u, static_cast<uint32_t>(_viewportExtent.height * _currentRenderScale));
	_renderResourceManager.on_window_resize(_drawExtent);
}
