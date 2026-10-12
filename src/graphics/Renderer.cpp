#include "Renderer.h"
#include <utils/vk_initializers.h>
#include <utils/vk_utils.h>
#include <cmath>
#include <algorithm>
#include <resources/Model.h>
#include <core/Components.h>
#include <resources/DefaultResources.h>
#include "graphics/GuiRenderer.h"
#include <core/Profiler.h>

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


	_renderTargetPool.init(_vulkanCore->get_device(), _vulkanCore->get_allocator());
	update_render_extent();

	_iblSystem.init(_vulkanCore, resourceManager);

	recreate_swapchain();
	create_command_buffers();
	create_sync_structures();

	dispatcher.sink<Events::WindowResizeEvent>().connect<&Renderer::on_window_resize>(this);

	// create compute passes
	_frustumCullPass = std::make_unique<FrustumCullPass>();
	_frustumCullPass->init(_vulkanCore, resourceManager);

	VkDescriptorSetLayout matLayout = _resourceManager->get_bindless_layout();

	// create render passes
	_shadowPass = std::make_unique<ShadowPass>();
	_shadowPass->init(_vulkanCore, resourceManager, matLayout, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT);

	_pointShadowPass = std::make_unique<PointShadowPass>();
	_pointShadowPass->init(_vulkanCore, resourceManager, matLayout, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT);

	_skyboxPass = std::make_unique<SkyboxPass>();
	_skyboxPass->init(_vulkanCore, resourceManager, matLayout, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT);

	_gbufferPass = std::make_unique<GBufferPass>();
	_gbufferPass->init(_vulkanCore, resourceManager, matLayout, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT);

	_deferredLightingPass = std::make_unique<DeferredLightingPass>();
	_deferredLightingPass->init(_vulkanCore, resourceManager, matLayout, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_UNDEFINED);

	_ssaoPass = std::make_unique<SSAOPass>();
	_ssaoPass->init(_vulkanCore, resourceManager, nullptr, VK_FORMAT_R8_UNORM, VK_FORMAT_UNDEFINED);

	_ssaoBlurPass = std::make_unique<SSAOBlurPass>();
	_ssaoBlurPass->init(_vulkanCore, resourceManager, nullptr, VK_FORMAT_R8_UNORM, VK_FORMAT_UNDEFINED);

	Profiler::get().init(_vulkanCore->get_device(), _vulkanCore->get_physical_device(), FRAME_OVERLAP);
}

void Renderer::cleanup(entt::dispatcher& dispatcher)
{
	dispatcher.sink<Events::WindowResizeEvent>().disconnect(this);

	vkDeviceWaitIdle(_vulkanCore->get_device());

	Profiler::get().cleanup();
	_renderTargetPool.cleanup();

	_frustumCullPass->cleanup();
	_pointShadowPass->cleanup();
	_shadowPass->cleanup();
	_skyboxPass->cleanup();
	_gbufferPass->cleanup();
	_deferredLightingPass->cleanup();
	_ssaoPass->cleanup();
	_ssaoBlurPass->cleanup();
	_iblSystem.cleanup();

	for (int i = 0; i < FRAME_OVERLAP; i++) {
		_frames[i]._cullStatsBuffer->unload();
		_frames[i].sceneDataBuffer->unload();
		_frames[i]._gpuObjectBuffer->unload();
		_frames[i]._gpuInstanceBuffer->unload();
		_frames[i]._gpuIndirectBuffer->unload();
		_frames[i]._gpuCountBuffer->unload();
		_frames[i]._materialDataBuffer->unload();

		_frames[i]._frameDescriptors.destroy_pools(_vulkanCore->get_device());

		vkDestroyCommandPool(_vulkanCore->get_device(), _frames[i]._commandPool, nullptr);
		vkDestroyFence(_vulkanCore->get_device(), _frames[i]._renderFence, nullptr);
		vkDestroySemaphore(_vulkanCore->get_device(), _frames[i]._swapchainSemaphore, nullptr);
	}

	_swapchain.cleanup(_vulkanCore->get_device());

	_vulkanCore->get_deletion_queue().flush_all();
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

	_viewportExtent = extent;
	update_render_extent();
}

void Renderer::render_frame(const entt::registry& registry, const GPUSceneData& sceneData)
{

	if (VkCommandBuffer cmd = begin_frame()) {
		DrawContext drawCtx;
		drawCtx.environment = _iblSystem.get_environment_resources();
		drawCtx.bindlessMaterialSet = _resourceManager->get_bindless_set();
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

		_renderGraph.init(&_renderTargetPool);
		_renderGraph.reset();
		_renderTargetPool.reset_frame(_frameNumber);

		RGBufferHandle objBuf = _renderGraph.import_buffer("GPUObjectBuffer", currentFrame._gpuObjectBuffer->get_buffer(), currentFrame._gpuObjectBuffer->get_size(), VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
		RGBufferHandle instBuf = _renderGraph.import_buffer("GPUInstanceBuffer", currentFrame._gpuInstanceBuffer->get_buffer(), currentFrame._gpuInstanceBuffer->get_size(), VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
		RGBufferHandle indirBuf = _renderGraph.import_buffer("GPUIndirectBuffer", currentFrame._gpuIndirectBuffer->get_buffer(), currentFrame._gpuIndirectBuffer->get_size(), VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
		RGBufferHandle countBuf = _renderGraph.import_buffer("GPUCountBuffer", currentFrame._gpuCountBuffer->get_buffer(), currentFrame._gpuCountBuffer->get_size(), VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
		RGBufferHandle statsBuf = _renderGraph.import_buffer("CullStatsBuffer", currentFrame._cullStatsBuffer->get_buffer(), currentFrame._cullStatsBuffer->get_size(), VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
		RGBufferHandle matBuf = _renderGraph.import_buffer("MaterialBuffer", currentFrame._materialDataBuffer->get_buffer(), currentFrame._materialDataBuffer->get_size(), VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);

		RGTextureDesc colorDesc{ .extent = {_drawExtent.width, _drawExtent.height, 1}, .format = VK_FORMAT_R16G16B16A16_SFLOAT, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
		RGTextureHandle sceneColor = _renderGraph.create_texture("SceneColor", colorDesc);

		RGTextureDesc depthDesc{ .extent = {_drawExtent.width, _drawExtent.height, 1}, .format = VK_FORMAT_D32_SFLOAT, .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT };
		RGTextureHandle sceneDepth = _renderGraph.create_texture("SceneDepth", depthDesc);

		RGTextureDesc shadowDesc{ .extent = {2048, 2048, 1}, .format = VK_FORMAT_D32_SFLOAT, .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT };
		RGTextureHandle shadowMap = _renderGraph.create_texture("ShadowMap", shadowDesc);

		RGTextureDesc pointShadowDesc{ .extent = {1024, 1024, 1}, .format = VK_FORMAT_D32_SFLOAT, .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .arrayLayers = 6, .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, .viewType = VK_IMAGE_VIEW_TYPE_CUBE };
		RGTextureHandle pointShadowMap = _renderGraph.create_texture("PointShadowMap", pointShadowDesc);

		RGTextureHandle gPosition = _renderGraph.create_texture("GBufferPosition", colorDesc);
		RGTextureHandle gNormal = _renderGraph.create_texture("GBufferNormal", colorDesc);
		RGTextureHandle gEmissive = _renderGraph.create_texture("GBufferEmissive", colorDesc);

		RGTextureDesc albedoDesc = colorDesc;
		albedoDesc.format = VK_FORMAT_R8G8B8A8_UNORM;
		RGTextureHandle gAlbedo = _renderGraph.create_texture("GBufferAlbedo", albedoDesc);
		RGTextureHandle gMetalRough = _renderGraph.create_texture("GBufferMetalRough", albedoDesc);

		RGTextureDesc ssaoDesc = colorDesc;
		ssaoDesc.format = VK_FORMAT_R8_UNORM;
		RGTextureHandle ssaoColor = _renderGraph.create_texture("SSAOColor", ssaoDesc);
		RGTextureHandle ssaoBlur = _renderGraph.create_texture("SSAOBlur", ssaoDesc);

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
				_pointShadowPass->render(rc, currentFrame, drawCtx, sceneData, params, _renderGraph);
			}
		);

		// geometry
		_renderGraph.add_pass<GeometryPassParameters>("GBufferPass",
			[&](RGPassBuilder& builder, GeometryPassParameters& params) {
				params.gpuObjectBuffer = builder.read_buffer(objBuf, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
				params.gpuIndirectBuffer = builder.read_buffer(indirBuf, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
				params.gpuInstanceBuffer = builder.read_buffer(instBuf, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
				params.gpuCountBuffer = builder.read_buffer(countBuf, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
				params.materialBuffer = builder.read_buffer(matBuf, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
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

				_renderGraph.transition_texture(graphCmd, params.sceneColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

				RGTextureHandle debugHandles[] = {
					sceneDepth,
					shadowMap,
					pointShadowMap,
					gPosition,
					gNormal,
					gAlbedo,
					gMetalRough,
					gEmissive,
					ssaoColor,
					ssaoBlur
				};
				// imgui requires shader read layout
				for (RGTextureHandle handle : debugHandles) {
					_renderGraph.transition_texture(graphCmd, handle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				}

				if (_guiRenderer) {
					_sceneColorDescriptor = _guiRenderer->get_image_descriptor(_renderGraph.get_image_view(params.sceneColor), DefaultResources::get().defaultSamplerLinear->get_sampler());
					_guiRenderer->render(graphCmd, _swapchain.get_image_views()[_swapchainImageIndex], _swapchain.get_extent());
				}
			}
		);

		_renderGraph.compile(_frameNumber);

		// register debug targets for imgui display
		_debugTargets.clear();
		auto register_debug_target = [&](const std::string& name, RGTextureHandle handle) {
			TargetInfo info;
			info.format = _renderGraph.get_format(handle);
			VkExtent3D ext = _renderGraph.get_extent(handle);
			info.extent = { ext.width, ext.height };
			info.view = _renderGraph.get_image_view(handle);
			info.isCube = _renderGraph.is_cube(handle);
			if (info.isCube) {
				for (int i = 0; i < 6; i++) info.faceViews.push_back(_renderGraph.get_image_view(handle, i));
			}
			_debugTargets[name] = info;
			};

		register_debug_target("SceneColor", sceneColor);
		register_debug_target("SceneDepth", sceneDepth);
		register_debug_target("ShadowMap", shadowMap);
		register_debug_target("PointShadowMap", pointShadowMap);
		register_debug_target("GBufferPosition", gPosition);
		register_debug_target("GBufferNormal", gNormal);
		register_debug_target("GBufferAlbedo", gAlbedo);
		register_debug_target("GBufferMetalRough", gMetalRough);
		register_debug_target("GBufferEmissive", gEmissive);
		register_debug_target("SSAOColor", ssaoColor);
		register_debug_target("SSAOBlur", ssaoBlur);

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

	if (_frameNumber >= FRAME_OVERLAP) { // flush safe to delete resources
		_vulkanCore->get_deletion_queue().flush_retired(_frameNumber - FRAME_OVERLAP);
	}
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
	VK_CHECK(vkResetCommandBuffer(cmd, 0)); // now that we are sure the commands finished executing, we can safely reset the command buffer to begin recording again
	VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);	// we will use this command buffer exactly once, so we want to let vulkan know that
	VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));	// start the command buffer recording

	Profiler::get().begin_frame(_frameNumber % FRAME_OVERLAP, cmd); // resolve previous frames gpu timestamp

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
	VK_CHECK(vkEndCommandBuffer(cmd));	// finalize command buffer for execution

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

	_vulkanCore->get_deletion_queue().next_frame(_frameNumber); // mark this frames resources as pending for deletion

	_isFrameStarted = false;
	_frameNumber++;

	Profiler::get().end_frame();

	VkResult presentResult = vkQueuePresentKHR(_vulkanCore->get_graphics_queue(), &presentInfo);
	if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR || _resizeRequested) {
		_resizeRequested = false;
		recreate_swapchain();
	}
	else {
		VK_CHECK(presentResult);
	}
}

void Renderer::extract_scene_data(const entt::registry& registry, DrawContext& ctx)
{
	FrameData& currentFrame = get_current_frame();

	if (_drawCache.isDirty) { // update draw cache if something changed
		_drawCache.opaqueObjects.clear();
		_drawCache.transparentObjects.clear();
		_drawCache.opaqueBatches.clear();

		uint32_t materialCount = 0;

		auto view = registry.view<const MeshComponent>();
		for (auto [entity, meshComp] : view.each()) {
			if (!meshComp.visible || !meshComp.mesh.is_valid()) {
				continue;
			}

			const auto& tc = registry.get<TransformComponent>(entity);
			glm::mat4 worldMatrix = tc.worldTransform;


			auto* matComp = registry.try_get<MaterialComponent>(entity);
			MaterialPassType passType = MaterialPassType::Opaque;
			bool isDoubleSided = false;

			if (matComp && matComp->material) {
				passType = (matComp->alphaMode == AlphaMode::Blend) ? MaterialPassType::Transparent : MaterialPassType::Opaque;
				isDoubleSided = matComp->doubleSided;
			}

			RenderObject obj;
			obj.entityId = static_cast<uint32_t>(entity);
			obj.indexCount = meshComp.count;
			obj.firstIndex = meshComp.startIndex + meshComp.mesh->get_first_index();
			obj.vertexBufferAddress = meshComp.mesh->get_vertex_buffer_address(); // address into global vertex buffer + offset for this mesh
			obj.bounds = meshComp.bounds;
			obj.transform = worldMatrix;
			obj.materialIndex = materialCount++;
			obj.passType = passType;
			obj.isDoubleSided = isDoubleSided;

			if (matComp && matComp->material) {
				auto paramData = matComp->material->get_parameter_data();
				size_t offset = obj.materialIndex * paramData.size();
				size_t requiredSize = (obj.materialIndex + 1) * paramData.size();

				if (currentFrame._materialDataBuffer->get_size() < requiredSize) {
					currentFrame._materialDataBuffer->resize(requiredSize * 1.5, true);
				}

				memcpy(static_cast<uint8_t*>(currentFrame._materialDataBuffer->get_mapped_data()) + offset, paramData.data(), paramData.size());
			}

			if (obj.passType == MaterialPassType::Transparent) {
				_drawCache.transparentObjects.push_back(obj);
			}
			else {
				_drawCache.opaqueObjects.push_back(obj);
			}
		}

		if (!_drawCache.opaqueObjects.empty()) {
			std::sort(_drawCache.opaqueObjects.begin(), _drawCache.opaqueObjects.end(), [](const RenderObject& a, const RenderObject b) {
				if (a.isDoubleSided != b.isDoubleSided)
					return a.isDoubleSided < b.isDoubleSided;

				if (a.firstIndex != b.firstIndex)
					return a.firstIndex < b.firstIndex;

				return a.indexCount < b.indexCount;
				});

			IndirectBatch currentBatch;
			currentBatch.first = 0;
			currentBatch.count = 0;
			currentBatch.batchIndex = 0;
			currentBatch.isDoubleSided = _drawCache.opaqueObjects[0].isDoubleSided;

			for (uint32_t i = 0; i < static_cast<uint32_t>(_drawCache.opaqueObjects.size()); i++) {
				const RenderObject& obj = _drawCache.opaqueObjects[i];
				const RenderObject& firstObj = _drawCache.opaqueObjects[currentBatch.first];

				bool isSameBatch = (obj.firstIndex == firstObj.firstIndex) &&
													 (obj.indexCount == firstObj.indexCount) &&
													 (obj.isDoubleSided == firstObj.isDoubleSided);

				if (!isSameBatch) {
					_drawCache.opaqueBatches.push_back(currentBatch);
					currentBatch.first = i;
					currentBatch.count = 0;
					currentBatch.batchIndex = static_cast<uint32_t>(_drawCache.opaqueBatches.size());
					currentBatch.isDoubleSided = obj.isDoubleSided;
				}
				currentBatch.count++;
			}
			_drawCache.opaqueBatches.push_back(currentBatch);
		}
		_drawCache.isDirty = false;
	}
	else 
	{
		for (auto& obj : _drawCache.opaqueObjects) {
			entt::entity e = static_cast<entt::entity>(obj.entityId);
			if (!registry.valid(e)) 
				continue;

			const auto& tc = registry.get<TransformComponent>(e);
			obj.transform = tc.worldTransform;

			auto* matComp = registry.try_get<MaterialComponent>(e);
			if (matComp && matComp->material) {
				auto paramData = matComp->material->get_parameter_data();
				size_t offset = obj.materialIndex * paramData.size();

				size_t requiredSize = (obj.materialIndex + 1) * paramData.size();
				if (currentFrame._materialDataBuffer->get_size() < requiredSize) {
					currentFrame._materialDataBuffer->resize(requiredSize * 1.5, true);
				}

				std::memcpy(static_cast<uint8_t*>(currentFrame._materialDataBuffer->get_mapped_data()) + offset, paramData.data(), paramData.size());

				//matComp->material->clear_dirty();
				//matComp->isDirty = false;
			}
		}
	}

	ctx.opaqueObjects = _drawCache.opaqueObjects;
	ctx.transparentObjects = _drawCache.transparentObjects; 
	ctx.opaqueBatches = _drawCache.opaqueBatches;
	ctx.indirectDrawCount = static_cast<uint32_t>(_drawCache.opaqueObjects.size());
}


void Renderer::recreate_swapchain()
{
	vkDeviceWaitIdle(_vulkanCore->get_device());

	// clear cached image descriptors in gui renderer so they will be recreated with new viewport extent
	if (_guiRenderer) {
		_guiRenderer->clear_image_descriptors();
	}
	_sceneColorDescriptor = VK_NULL_HANDLE;

	VkExtent2D actualExtent = _pendingResizeExtent;
	if (actualExtent.width == 0 || actualExtent.height == 0) {
		actualExtent = _window->get_extent();
		if (actualExtent.width == 0 || actualExtent.height == 0)
			return; // window minimized
	}

	_swapchain.destroy_swapchain();
	_swapchain.init(_vulkanCore->get_physical_device(), _vulkanCore->get_device(), _vulkanCore->get_surface(), actualExtent.width, actualExtent.height);

	_debugTargets.clear();
	update_render_extent();
	_renderTargetPool.cleanup();
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


		_frames[i]._materialDataBuffer = _resourceManager->load<Buffer>("materialData" + fi, sizeof(MaterialConstants) * 1000, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	}
}

void Renderer::update_render_extent()
{
	float scale = std::clamp(static_cast<float>(cvRenderScale.Get()), 0.1f, 1.0f);

	_currentRenderScale = scale;
	_drawExtent.width = std::max(1u, static_cast<uint32_t>(_viewportExtent.width * _currentRenderScale));
	_drawExtent.height = std::max(1u, static_cast<uint32_t>(_viewportExtent.height * _currentRenderScale));
}
