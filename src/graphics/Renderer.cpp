#include "Renderer.h"
#include <utils/vk_initializers.h>
#include <utils/vk_utils.h>
#include <cmath>
#include <algorithm>
#include <resources/Model.h>
#include <core/Components.h>
#include <resources/DefaultResources.h>
#include "graphics/GuiRenderer.h"

AutoCVar_Float cvRenderScale("r.renderScale", "Scale multiplier for rendering resolution", 1.0, 0.1, 1.0, CVarFlags::EditFloatSlider);

AutoCVar_Int cvSSAO("r.ssao", "Enable Screen Space Ambient Occlusion", 1, CVarFlags::EditCheckBox);
AutoCVar_Float cvSSAORadius("r.ssao.radius", "SSAO sample hemisphere radius", 0.5f, 0.05, 3.0, CVarFlags::EditFloatSlider);
AutoCVar_Float cvSSAOBias("r.ssao.bias", "SSAO depth comparison bias", 0.025f, 0.001, 0.1, CVarFlags::EditFloatSlider);
AutoCVar_Float cvSSAOPower("r.ssao.power", "SSAO occlusion power/intensity", 1.5f, 0.1, 5.0, CVarFlags::EditFloatSlider);

void Renderer::init(Window* window, VulkanCore* vulkanCore, entt::dispatcher& dispatcher, ResourceManager* resourceManager)
{
	_window = window;
	_vulkanCore = vulkanCore;
	_resourceManager = resourceManager;

	_viewportExtent = _window->get_extent();

	_iblSystem.init(_vulkanCore, resourceManager);

	//DefaultResources defaultResources(resourceManager);

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
	_shadowPass->init(_vulkanCore, resourceManager, matLayout, _renderTargets.find("SceneColor")->second->get_format(), _renderTargets.find("SceneDepth")->second->get_format());

	_pointShadowPass = std::make_unique<PointShadowPass>();
	_pointShadowPass->init(_vulkanCore, resourceManager, matLayout, _renderTargets.find("SceneColor")->second->get_format(), VK_FORMAT_D32_SFLOAT);

	_skyboxPass = std::make_unique<SkyboxPass>();
	_skyboxPass->init(_vulkanCore, resourceManager, matLayout, _renderTargets.find("SceneColor")->second->get_format(), _renderTargets.find("SceneDepth")->second->get_format());

	_gbufferPass = std::make_unique<GBufferPass>();
	_gbufferPass->init(_vulkanCore, resourceManager, matLayout, _renderTargets.find("SceneColor")->second->get_format(), _renderTargets.find("SceneDepth")->second->get_format());

	_deferredLightingPass = std::make_unique<DeferredLightingPass>();
	_deferredLightingPass->init(_vulkanCore, resourceManager, matLayout, _renderTargets.find("SceneColor")->second->get_format(), VK_FORMAT_UNDEFINED);

	_ssaoPass = std::make_unique<SSAOPass>();
	_ssaoPass->init(_vulkanCore, resourceManager, nullptr, _renderTargets.find("SSAOColor")->second->get_format(), VK_FORMAT_UNDEFINED);

	_ssaoBlurPass = std::make_unique<SSAOBlurPass>();
	_ssaoBlurPass->init(_vulkanCore, resourceManager, nullptr, _renderTargets.find("SSAOBlur")->second->get_format(), VK_FORMAT_UNDEFINED);
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

	//for (auto& [name, tex] : _renderTargets) {
	//	tex->unload();
	//}
	_renderTargets.clear();

	for (int i = 0; i < FRAME_OVERLAP; i++) {
		_frames[i]._cullStatsBuffer->unload();
		_frames[i].sceneDataBuffer->unload();
		_frames[i]._gpuObjectBuffer->unload();
		_frames[i]._gpuInstanceBuffer->unload();
		_frames[i]._gpuIndirectBuffer->unload();
		_frames[i]._gpuCountBuffer->unload();
		_frames[i]._materialDataBuffer->unload();


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

	create_render_targets();

	// update the descriptor 
	if (_guiRenderer) {
		VkImageView newView = _renderTargets.find("SceneColor")->second->get_image_view();
		_guiRenderer->get_image_descriptor(newView, DefaultResources::get().defaultSamplerLinear->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
		currentFrame.sceneDataBuffer->resize(sizeof(GPUSceneData));
		currentFrame.sceneDataBuffer->copy_data(&sceneData, sizeof(GPUSceneData));

		uint32_t drawCount = std::max(1u, static_cast<uint32_t>(drawCtx.opaqueObjects.size()));
		uint32_t batchCount = std::max(1u, static_cast<uint32_t>(drawCtx.opaqueBatches.size()));
		currentFrame._gpuObjectBuffer->resize(sizeof(GPUObjectData) * drawCount);
		currentFrame._gpuInstanceBuffer->resize(sizeof(uint32_t) * drawCount);
		currentFrame._gpuIndirectBuffer->resize(sizeof(VkDrawIndexedIndirectCommand) * drawCount);
		currentFrame._gpuCountBuffer->resize(sizeof(uint32_t) * batchCount);

		_renderGraph.reset();

		RGBufferHandle objBuf = _renderGraph.import_buffer("GPUObjectBuffer", currentFrame._gpuObjectBuffer->get_buffer(), currentFrame._gpuObjectBuffer->get_size(), VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
		RGBufferHandle instBuf = _renderGraph.import_buffer("GPUInstanceBuffer", currentFrame._gpuInstanceBuffer->get_buffer(), currentFrame._gpuInstanceBuffer->get_size(), VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
		RGBufferHandle indirBuf = _renderGraph.import_buffer("GPUIndirectBuffer", currentFrame._gpuIndirectBuffer->get_buffer(), currentFrame._gpuIndirectBuffer->get_size(), VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
		RGBufferHandle countBuf = _renderGraph.import_buffer("GPUCountBuffer", currentFrame._gpuCountBuffer->get_buffer(), currentFrame._gpuCountBuffer->get_size(), VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
		RGBufferHandle statsBuf = _renderGraph.import_buffer("CullStatsBuffer", currentFrame._cullStatsBuffer->get_buffer(), currentFrame._cullStatsBuffer->get_size(), VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);

		auto import_tex = [&](const std::string& name) {
			auto& tex = _renderTargets.at(name);
			return _renderGraph.import_texture(name, tex->get_image(), tex->get_image_view(), tex->get_format(), tex->get_extent(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			};

		RGTextureHandle sceneColor = import_tex("SceneColor");
		RGTextureHandle sceneDepth = import_tex("SceneDepth");
		RGTextureHandle shadowMap = import_tex("ShadowMap");
		RGTextureHandle pointShadowMap = import_tex("PointShadowMap");
		RGTextureHandle gPosition = import_tex("GBufferPosition");
		RGTextureHandle gNormal = import_tex("GBufferNormal");
		RGTextureHandle gAlbedo = import_tex("GBufferAlbedo");
		RGTextureHandle gMetalRough = import_tex("GBufferMetalRough");
		RGTextureHandle gEmissive = import_tex("GBufferEmissive");
		RGTextureHandle ssaoColor = import_tex("SSAOColor");
		RGTextureHandle ssaoBlur = import_tex("SSAOBlur");
		RGTextureHandle skybox = import_tex("SkyCubemap");

		// frustum cull
		_renderGraph.add_pass<FrustumCullPassParameters>("FrustumCull",
			[&](RGPassBuilder& builder, FrustumCullPassParameters& params) {
				params.gpuObjectBuffer = builder.read_buffer(objBuf, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
				params.gpuInstanceBuffer = builder.write_buffer(instBuf, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
				params.gpuIndirectBuffer = builder.write_buffer(indirBuf, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
				params.gpuCountBuffer = builder.write_buffer(countBuf, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
				params.cullStatsBuffer = builder.write_buffer(statsBuf, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
			},
			[=, this, &currentFrame, &drawCtx, &sceneData](const FrustumCullPassParameters& params, RenderContext& rc) {
				_frustumCullPass->dispatch(rc, currentFrame, drawCtx, sceneData, _drawExtent, params, _renderGraph);
			}
		);

		// shadow map 
		_renderGraph.add_pass<ShadowPassParameters>("ShadowPass",
			[&](RGPassBuilder& builder, ShadowPassParameters& params) {
				params.gpuObjectBuffer = builder.read_buffer(objBuf, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
				params.gpuIndirectBuffer = builder.read_buffer(indirBuf, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
				params.gpuInstanceBuffer = builder.read_buffer(instBuf, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
				params.gpuCountBuffer = builder.read_buffer(countBuf, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);

				params.shadowMap = builder.set_depth_attachment(shadowMap, VK_ATTACHMENT_LOAD_OP_CLEAR, 1.0f);
				builder.set_extent({ 2048, 2048 });
			},
			[=, this, &currentFrame, &drawCtx, &sceneData](const ShadowPassParameters& params, RenderContext& rc) {
				_shadowPass->render(rc, currentFrame, drawCtx, sceneData, params, _renderGraph);
			}
		);

		// point shadow
		_renderGraph.add_pass<PointShadowPassParameters>("PointShadowPass",
			[&](RGPassBuilder& builder, PointShadowPassParameters& params) {
				params.gpuObjectBuffer = builder.read_buffer(objBuf, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
				params.gpuIndirectBuffer = builder.read_buffer(indirBuf, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
				params.gpuInstanceBuffer = builder.read_buffer(instBuf, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
				params.gpuCountBuffer = builder.read_buffer(countBuf, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);

				params.pointShadowMap = builder.write_texture(pointShadowMap, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
			},
			[=, this, &currentFrame, &drawCtx, &sceneData](const PointShadowPassParameters& params, RenderContext& rc) {
				_pointShadowPass->render(rc, currentFrame, _renderTargets, drawCtx, sceneData, params, _renderGraph);
			}
		);

		// geometry
		_renderGraph.add_pass<GeometryPassParameters>("GBufferPass",
			[&](RGPassBuilder& builder, GeometryPassParameters& params) {
				params.gpuObjectBuffer = builder.read_buffer(objBuf, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
				params.gpuIndirectBuffer = builder.read_buffer(indirBuf, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
				params.gpuInstanceBuffer = builder.read_buffer(instBuf, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
				params.gpuCountBuffer = builder.read_buffer(countBuf, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);

				params.gPosition = builder.set_color_attachment(gPosition, VK_ATTACHMENT_LOAD_OP_CLEAR);
				params.gNormal = builder.set_color_attachment(gNormal, VK_ATTACHMENT_LOAD_OP_CLEAR);
				params.gAlbedo = builder.set_color_attachment(gAlbedo, VK_ATTACHMENT_LOAD_OP_CLEAR);
				params.gMetalRough = builder.set_color_attachment(gMetalRough, VK_ATTACHMENT_LOAD_OP_CLEAR);
				params.gEmissive = builder.set_color_attachment(gEmissive, VK_ATTACHMENT_LOAD_OP_CLEAR);
				params.depth = builder.set_depth_attachment(sceneDepth, VK_ATTACHMENT_LOAD_OP_CLEAR, 0.0f);
				builder.set_extent(_drawExtent);
			},
			[=, this, &currentFrame, &drawCtx, &sceneData](const GeometryPassParameters& params, RenderContext& rc) {
				_gbufferPass->render(rc, currentFrame, drawCtx, sceneData, params, _renderGraph);
			}
		);

		// ssao
		_renderGraph.add_pass<SSAOPassParameters>("SSAOPass",
			[&](RGPassBuilder& builder, SSAOPassParameters& params) {
				params.gPosition = builder.read_texture(gPosition, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				params.gNormal = builder.read_texture(gNormal, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				params.ssao = builder.set_color_attachment(ssaoColor, VK_ATTACHMENT_LOAD_OP_CLEAR, { 1.0f, 1.0f, 1.0f, 1.0f });
				builder.set_extent(_drawExtent);
			},
			[=, this, &currentFrame, &drawCtx, &sceneData](const SSAOPassParameters& params, RenderContext& rc) {
				_ssaoPass->render(rc, currentFrame, drawCtx, sceneData, params, _renderGraph);
			}
		);

		// ssao blur
		_renderGraph.add_pass<SSAOBlurPassParameters>("SSAOBlurPass",
			[&](RGPassBuilder& builder, SSAOBlurPassParameters& params) {
				params.ssao = builder.read_texture(ssaoColor, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				params.ssaoBlur = builder.set_color_attachment(ssaoBlur, VK_ATTACHMENT_LOAD_OP_CLEAR, { 1.0f, 1.0f, 1.0f, 1.0f });
				builder.set_extent(_drawExtent);
			},
			[=, this, &currentFrame, &drawCtx, &sceneData](const SSAOBlurPassParameters& params, RenderContext& rc) {
				_ssaoBlurPass->render(rc, currentFrame, drawCtx, sceneData, params, _renderGraph);
			}
		);

		// deferred lighting
		_renderGraph.add_pass<DeferredPassParameters>("DeferredLightingPass",
			[&](RGPassBuilder& builder, DeferredPassParameters& params) {
				params.gPosition = builder.read_texture(gPosition, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				params.gNormal = builder.read_texture(gNormal, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				params.gAlbedo = builder.read_texture(gAlbedo, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				params.gMetalRough = builder.read_texture(gMetalRough, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				params.gEmissive = builder.read_texture(gEmissive, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				params.shadowMap = builder.read_texture(shadowMap, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				params.pointShadowMap = builder.read_texture(pointShadowMap, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				params.ssao = builder.read_texture(ssaoBlur, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

				params.sceneColor = builder.set_color_attachment(sceneColor, VK_ATTACHMENT_LOAD_OP_CLEAR, { 0.0f, 0.0f, 0.0f, 1.0f });
				builder.set_extent(_drawExtent);
			},
			[=, this, &currentFrame, &drawCtx, &sceneData](const DeferredPassParameters& params, RenderContext& rc) {
				_deferredLightingPass->render(rc, currentFrame, drawCtx, sceneData, params, _renderGraph);
			}
		);

		// skybox
		_renderGraph.add_pass<SkyboxPassParameters>("SkyboxPass",
			[&](RGPassBuilder& builder, SkyboxPassParameters& params) {
				params.sceneDepth = builder.read_texture(sceneDepth, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
				params.sceneColor = builder.set_color_attachment(sceneColor, VK_ATTACHMENT_LOAD_OP_LOAD);
				params.sceneDepth = builder.set_depth_attachment(sceneDepth, VK_ATTACHMENT_LOAD_OP_LOAD);
				builder.set_extent(_drawExtent);
			},
			[=, this, &currentFrame, &drawCtx, &sceneData](const SkyboxPassParameters& params, RenderContext& rc) {
				_skyboxPass->render(rc, currentFrame, drawCtx, sceneData, params, _renderGraph);
			}
		);

		// swapchain, set side effect to guaraantee execution
		struct PresentPassParams { RGTextureHandle sceneColor; };
		_renderGraph.add_pass<PresentPassParams>("PresentComposite",
			[&](RGPassBuilder& builder, PresentPassParams& params) {
				params.sceneColor = builder.read_texture(sceneColor, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT);
				builder.set_side_effect(true); 
			},
			[=, this](const PresentPassParams& params, RenderContext& rc) {
				VkCommandBuffer graphCmd = rc.get_cmd();
				VkImage swapchainImage = _swapchain.get_images()[_swapchainImageIndex];

				vkutil::image_mem_barrier2(graphCmd, swapchainImage, _swapchain.get_image_format(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
				vkutil::copy_image_to_image(graphCmd, _renderGraph.get_image(params.sceneColor), swapchainImage, _drawExtent, _swapchain.get_extent());
				vkutil::image_mem_barrier2(graphCmd, swapchainImage, _swapchain.get_image_format(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

				for (const auto& [name, tex] : _renderTargets) {
					RGTextureHandle handle = _renderGraph.find_texture(name);
					if (handle.is_valid()) {
						_renderGraph.transition_texture(graphCmd, handle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
					}
				}

				if (_guiRenderer) {
					_guiRenderer->render(graphCmd, _swapchain.get_image_views()[_swapchainImageIndex], _swapchain.get_extent());
				}
			}
		);

		_renderGraph.compile();
		_renderGraph.execute(ctx);

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

	VkExtent2D sceneColorExtent = _renderTargets.find("SceneColor")->second->get_extent_2d();
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
	vkutil::image_mem_barrier2(cmd, currentSwapchainImage, _swapchain.get_image_format(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 1, 1, 0);

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

/*
void Renderer::blit_to_swapchain(VkCommandBuffer cmd)
{
	VkImage swapchainImage = _swapchain.get_images()[_swapchainImageIndex];
	ResourceHandle<Texture> sceneColor = _renderTargets.find("SceneColor")->second;

	// transition the draw image and the swapchain image into their correct transfer layouts
	// so we can blit from the draw image to the swapchain image
	//_renderGraph.transition_texture(cmd, "SceneColor", ResourceState::TransferSrc);
	_renderGraph.transition_texture(cmd, "SceneColor", VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

	// transition swapchain image to transferdst to copy into it
	vkutil::image_mem_barrier2(cmd, swapchainImage, _swapchain.get_image_format(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);


	// copy SceneColor into the swapchain
	vkutil::copy_image_to_image(cmd, sceneColor->get_image(), swapchainImage, _drawExtent, _swapchain.get_extent());

	// set swapchain image layout to color attachment optimal for imgui 
	vkutil::image_mem_barrier2(cmd, swapchainImage, _swapchain.get_image_format(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}
*/

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
	size_t capacity = currentFrame._materialDataBuffer->get_size() / sizeof(MaterialConstants); // material data buffer capacity
	uint32_t materialCount = 0;
	MaterialConstants* matDataMap = static_cast<MaterialConstants*>(currentFrame._materialDataBuffer->get_mapped_data()); // pointer to the buffers mapped data

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
				currentFrame._materialDataBuffer->resize((capacity + 1000) * sizeof(MaterialConstants), true);
				capacity = currentFrame._materialDataBuffer->get_size() / sizeof(MaterialConstants);
				matDataMap = static_cast<MaterialConstants*>(currentFrame._materialDataBuffer->get_mapped_data()); // update the pointer to the buffers mapped data
			}

			matDataMap[materialCount] = get_material_constants(*matComp); // copy material constants into the buffer
			size_t bufferOffset = sizeof(MaterialConstants) * materialCount; // offset in the buffer for this material
			materialCount++; // increment material count for next material

			descriptorSet = currentFrame._frameDescriptors.allocate(_vulkanCore->get_device(), Material::get_descriptor_layout());

			DescriptorWriter writer;
			writer.write_buffer(0, currentFrame._materialDataBuffer->get_buffer(), sizeof(MaterialConstants), bufferOffset, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);

			VkImageView albedoView = matComp->albedoTexture.is_valid() ? matComp->albedoTexture->get_image_view() : DefaultResources::get().greyImage->get_image_view();
			VkSampler albedoSampler = matComp->albedoTexture.is_valid() ? matComp->albedoTexture->get_sampler() : DefaultResources::get().defaultSamplerLinear->get_sampler();

			VkImageView metalRoughView = matComp->metallicRoughnessTexture.is_valid() ? matComp->metallicRoughnessTexture->get_image_view() : DefaultResources::get().whiteImage->get_image_view();
			VkSampler metalRoughSampler = matComp->metallicRoughnessTexture.is_valid() ? matComp->metallicRoughnessTexture->get_sampler() : DefaultResources::get().defaultSamplerLinear->get_sampler();

			VkImageView occlusionView = matComp->occlusionTexture.is_valid() ? matComp->occlusionTexture->get_image_view() : DefaultResources::get().whiteImage->get_image_view();
			VkSampler occlusionSampler = matComp->occlusionTexture.is_valid() ? matComp->occlusionTexture->get_sampler() : DefaultResources::get().defaultSamplerLinear->get_sampler();

			VkImageView normalView = matComp->normalTexture.is_valid() ? matComp->normalTexture->get_image_view() : DefaultResources::get().defaultNormalImage->get_image_view();
			VkSampler normalSampler = matComp->normalTexture.is_valid() ? matComp->normalTexture->get_sampler() : DefaultResources::get().defaultSamplerLinear->get_sampler();

			VkImageView emissiveView = matComp->emissiveTexture.is_valid() ? matComp->emissiveTexture->get_image_view() : DefaultResources::get().blackImage->get_image_view();
			VkSampler emissiveSampler = matComp->emissiveTexture.is_valid() ? matComp->emissiveTexture->get_sampler() : DefaultResources::get().defaultSamplerLinear->get_sampler();

			writer.write_image(1, albedoView, albedoSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			writer.write_image(2, metalRoughView, metalRoughSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			writer.write_image(3, occlusionView, occlusionSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			writer.write_image(4, normalView, normalSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			writer.write_image(5, emissiveView, emissiveSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

			writer.update_set(_vulkanCore->get_device(), descriptorSet); // update descriptor set for this material
			matComp->isDirty = false;
		}
		else { // no material component for this entity
			if (materialCount >= capacity) {
				currentFrame._materialDataBuffer->resize((capacity + 1000) * sizeof(MaterialConstants), true);
				capacity = currentFrame._materialDataBuffer->get_size() / sizeof(MaterialConstants);
				matDataMap = static_cast<MaterialConstants*>(currentFrame._materialDataBuffer->get_mapped_data());
			}

			// write default material constants into the mapped data
			MaterialConstants defaultConstants{};
			defaultConstants.colorFactors = glm::vec4(1.0f);
			defaultConstants.metalRoughFactors = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
			defaultConstants.extraFactors = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
			defaultConstants.emissiveFactor = glm::vec4(0.0f);

			matDataMap[materialCount] = defaultConstants;
			size_t bufferOffset = sizeof(MaterialConstants) * materialCount;
			materialCount++;

			// allocate descriptor set and write the buffer offset
			descriptorSet = currentFrame._frameDescriptors.allocate(_vulkanCore->get_device(), Material::get_descriptor_layout());
			DescriptorWriter writer;
			writer.write_buffer(0, currentFrame._materialDataBuffer->get_buffer(), sizeof(MaterialConstants), bufferOffset, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
			writer.write_image(1, DefaultResources::get().greyImage->get_image_view(), DefaultResources::get().defaultSamplerLinear->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			writer.write_image(2, DefaultResources::get().whiteImage->get_image_view(), DefaultResources::get().defaultSamplerLinear->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			writer.write_image(3, DefaultResources::get().whiteImage->get_image_view(), DefaultResources::get().defaultSamplerLinear->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			writer.write_image(4, DefaultResources::get().defaultNormalImage->get_image_view(), DefaultResources::get().defaultSamplerLinear->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			writer.write_image(5, DefaultResources::get().blackImage->get_image_view(), DefaultResources::get().defaultSamplerLinear->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
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

	create_render_targets();

	if (_guiRenderer) {
		VkImageView newView = _renderTargets.find("SceneColor")->second->get_image_view();
		//_guiRenderer->get_image_descriptor(newView, _vulkanCore->_defaultSamplerLinear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		_guiRenderer->get_image_descriptor(newView, DefaultResources::get().defaultSamplerLinear->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
}

VkDescriptorSet Renderer::get_target_descriptor(const std::string& name, uint32_t faceIndex)
{
	if (!_guiRenderer || _renderTargets.find(name) == _renderTargets.end()) {
		return VK_NULL_HANDLE;
	}
	const ResourceHandle<Texture> tex = _renderTargets.at(name);
	VkImageView targetView = (faceIndex < tex->get_face_views().size()) ? tex->get_face_view(faceIndex) : tex->get_image_view();
	//return _guiRenderer->get_image_descriptor(targetView, _vulkanCore->_defaultSamplerLinear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	return _guiRenderer->get_image_descriptor(targetView, DefaultResources::get().defaultSamplerLinear->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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

		std::string fi = std::to_string(i);
		_frames[i]._cullStatsBuffer = _resourceManager->load<Buffer>("cullStats" + fi, sizeof(CullStatsData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

		_frames[i].sceneDataBuffer = _resourceManager->load<Buffer>("sceneData" + fi, sizeof(GPUSceneData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);


		_frames[i]._gpuObjectBuffer = _resourceManager->load<Buffer>("gpuObject" + fi, sizeof(GPUObjectData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
		_frames[i]._gpuInstanceBuffer = _resourceManager->load<Buffer>("gpuInstance" + fi, sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		_frames[i]._gpuIndirectBuffer = _resourceManager->load<Buffer>("gpuIndirect" + fi, sizeof(VkDrawIndexedIndirectCommand), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
		_frames[i]._gpuCountBuffer = _resourceManager->load<Buffer>("gpuCount" + fi, sizeof(MaterialConstants), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);


		_frames[i]._materialDataBuffer = _resourceManager->load<Buffer>("materialData" + fi, sizeof(MaterialConstants) * 1000, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	}
}

void Renderer::create_render_targets()
{
	for (auto& [name, handle] : _renderTargets) {
		_resourceManager->force_unload<Texture>(name);
	}
	_renderTargets.clear();

	float scale = std::clamp(static_cast<float>(cvRenderScale.Get()), 0.1f, 1.0f);
	_currentRenderScale = scale;
	_drawExtent.width = std::max(1u, static_cast<uint32_t>(_viewportExtent.width * _currentRenderScale));
	_drawExtent.height = std::max(1u, static_cast<uint32_t>(_viewportExtent.height * _currentRenderScale));

	VkExtent2D targetExtent = _drawExtent;
	if (targetExtent.width == 0 || targetExtent.height == 0) {
		targetExtent = _window->get_extent();
	}

	auto make_target = [&](const std::string& name, VkFormat format, VkImageUsageFlags usage, VkExtent3D ext, int layers = 1, VkImageCreateFlags flags = 0, VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D) {
		TextureDescription desc{
			.extent = ext,
			.format = format,
			.usage = usage,
			.mipLevels = 1,
			.arrayLayers = (uint32_t)layers,
			.flags = flags,
			.viewType = viewType,
			.aspectMask = static_cast<VkImageAspectFlags>((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT) 
		};
		_renderTargets[name] = _resourceManager->load<Texture>(name, desc);

		};

	make_target("SceneColor", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, { _drawExtent.width, _drawExtent.height, 1 });
	make_target("SceneDepth", VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, { _drawExtent.width, _drawExtent.height, 1 });
	make_target("ShadowMap", VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, { 2048, 2048, 1 });
	make_target("PointShadowMap", VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, { 1024, 1024, 1 }, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, VK_IMAGE_VIEW_TYPE_CUBE);
	make_target("SkyCubemap", VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, { 256, 256, 1 }, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, VK_IMAGE_VIEW_TYPE_CUBE);
	make_target("GBufferPosition", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, { _drawExtent.width, _drawExtent.height, 1 });
	make_target("GBufferNormal", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, { _drawExtent.width, _drawExtent.height, 1 });
	make_target("GBufferAlbedo", VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, { _drawExtent.width, _drawExtent.height, 1 });
	make_target("GBufferMetalRough", VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, { _drawExtent.width, _drawExtent.height, 1 });
	make_target("GBufferEmissive", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, { _drawExtent.width, _drawExtent.height, 1 });
	make_target("SSAOColor", VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, { _drawExtent.width, _drawExtent.height, 1 });
	make_target("SSAOBlur", VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, { _drawExtent.width, _drawExtent.height, 1 });

	_vulkanCore->immediate_submit([&](VkCommandBuffer cmd) {
		for (const auto& [name, texHandle] : _renderTargets) {
			uint32_t layers = texHandle->get_face_views().empty() ? 1 : static_cast<uint32_t>(texHandle->get_face_views().size());
			vkutil::image_mem_barrier2(cmd, texHandle->get_image(), texHandle->get_format(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, layers, 1, 0);
		}
		});
}

void Renderer::update_render_extent()
{
	float scale = std::clamp(static_cast<float>(cvRenderScale.Get()), 0.1f, 1.0f);

	// Only recreate if the scale actually changed
	if (std::abs(scale - _currentRenderScale) > 0.001f) {
		vkDeviceWaitIdle(_vulkanCore->get_device());
		if (_guiRenderer) {
			_guiRenderer->clear_image_descriptors();
		}

		_currentRenderScale = scale;
		_drawExtent.width = std::max(1u, static_cast<uint32_t>(_viewportExtent.width * _currentRenderScale));
		_drawExtent.height = std::max(1u, static_cast<uint32_t>(_viewportExtent.height * _currentRenderScale));

		create_render_targets();

		if (_guiRenderer) {
			
			VkImageView newView = _renderTargets.at("SceneColor")->get_image_view();
			//_guiRenderer->get_image_descriptor(newView, _vulkanCore-_defaultSamplerLinear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			_guiRenderer->get_image_descriptor(newView, DefaultResources::get().defaultSamplerLinear->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}
	}
}
