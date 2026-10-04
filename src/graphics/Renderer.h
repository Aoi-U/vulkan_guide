#pragma once
#include <core/vk_types.h>
#include <graphics/VulkanCore.h>
#include <graphics/Swapchain.h>

#include <graphics/pass/FrustumCullpass.h>
#include <graphics/pass/ForwardPass.h>
#include <graphics/pass/ShadowPass.h>
#include <graphics/pass/SkyboxPass.h>
#include <graphics/pass/PointShadowPass.h>
#include <graphics/pass/GBufferPass.h>
#include <graphics/pass/DeferredLightingPass.h>
#include <graphics/pass/SSAOPass.h>
#include <graphics/pass/SSAOBlurPass.h>

#include <graphics/MaterialSystem.h>
#include <graphics/IBLSystem.h>
#include <graphics/RenderResourceManager.h>
#include <window/window.h>
#include <utils/vk_descriptors.h>
#include <memory>
#include <entt.hpp>
#include <vector>
#include <core/Events.h>
#include <core/cvars.h>

extern AutoCVar_Int cvRenderPath;

extern AutoCVar_Int cvSSAO;
extern AutoCVar_Float cvSSAORadius;
extern AutoCVar_Float cvSSAOBias;
extern AutoCVar_Float cvSSAOPower;

class GuiRenderer;

struct IndirectBatch
{
	VkBuffer indexBuffer;
	MaterialInstance* material;
	uint32_t first; // first index in the indirect buffer
	uint32_t count; // number of indirect commands in this batch
	uint32_t batchIndex; // index of this batch in the batch array
};

struct DrawContext
{
	std::vector<RenderObject> opaqueObjects;
	std::vector<RenderObject> transparentObjects;

	std::vector<IndirectBatch> opaqueBatches;

	// indirect draw 
	uint32_t indirectDrawCount{ 0 };

	EnvironmentResources environment{}; // environment resources for the scene

	void clear() {
		opaqueObjects.clear();
		transparentObjects.clear();
		opaqueBatches.clear();

		indirectDrawCount = 0;
	}
};

struct FrameData
{
	VkCommandPool _commandPool;
	VkCommandBuffer _mainCommandBuffer;

	VkSemaphore _swapchainSemaphore;
	VkFence _renderFence;

	DeletionQueue _deletionQueue;
	DescriptorAllocatorGrowable _frameDescriptors;


	AllocatedBuffer sceneDataBuffer; // uniform buffer for scene data

	// indirect draw buffers
	AllocatedBuffer _cullStatsBuffer; // buffer for frustum cull statistics
	AllocatedBuffer _gpuObjectBuffer; // buffer for object data used in frustum culling
	AllocatedBuffer _gpuInstanceBuffer; // buffer for instance ids for visible objects
	AllocatedBuffer _gpuIndirectBuffer; // buffer for indirect draw commands
	AllocatedBuffer _gpuCountBuffer; // buffer for draw counts per batch
};

enum class RenderPath : uint32_t
{
	Forward = 0,
	Deferred = 1
};

class Renderer
{
public:
	static constexpr unsigned int FRAME_OVERLAP = 2;

	void init(Window* window, VulkanCore* vulkanCore, MaterialSystem* materialSystem, entt::dispatcher& dispatcher);

	void cleanup(entt::dispatcher& dispatcher);

	void on_window_resize(const Events::WindowResizeEvent& event);

	void set_gui_renderer(GuiRenderer* guiRenderer) { _guiRenderer = guiRenderer; }
	void set_viewport_extent(VkExtent2D extent);
	void set_render_path(RenderPath path) { _renderPath = path; }

	void render_frame(const entt::registry& registry, const GPUSceneData& sceneData);

	void recreate_swapchain();

	bool load_environment(const std::filesystem::path& path) { return _iblSystem.load_hdr(path); }
	void remove_environment() { _iblSystem.load_procedural_default(); }

	VkExtent2D get_swapchain_extent() const { return _swapchain.get_extent(); }
	VkExtent2D get_draw_extent() const { return _drawExtent; }
	VkImageView get_current_swapchain_image_view() const {
		return _swapchain.get_image_views()[_swapchainImageIndex];
	}
	VkFormat get_swapchain_format() const { return _swapchain.get_image_format(); }
	FrameData& get_current_frame() { return _frames[_frameNumber % FRAME_OVERLAP]; }
	RenderResourceManager& get_resources() { return _resourceManager; }
	IBLSystem& get_ibl() { return _iblSystem; }
	GuiRenderer* get_gui_renderer() const { return _guiRenderer; }
	RenderPath get_render_path() const { return _renderPath; }
	VkDescriptorSet get_scene_color_descriptor();
	VkExtent2D get_viewport_extent() const { return _viewportExtent; }
	const std::vector<PassStats> get_stats() const;

private:
	Window* _window;
	VulkanCore* _vulkanCore;
	Swapchain _swapchain;
	
	RenderResourceManager _resourceManager;
	VkExtent2D _drawExtent;
	VkExtent2D _viewportExtent{ 1280, 720 };

	FrameData _frames[FRAME_OVERLAP];
	uint32_t _frameNumber{ 0 };
	uint32_t _swapchainImageIndex{ 0 };
	bool _isFrameStarted{ false };

	bool _resizeRequested{ false };
	VkExtent2D _pendingResizeExtent{ 0, 0 };

	GuiRenderer* _guiRenderer{ nullptr };

	RenderPath _renderPath{ RenderPath::Deferred };

	std::unique_ptr<FrustumCullPass> _frustumCullPass;
	std::unique_ptr<ForwardPass> _forwardPass;
	std::unique_ptr<ShadowPass> _shadowPass;
	std::unique_ptr<PointShadowPass> _pointShadowPass;
	std::unique_ptr<SkyboxPass> _skyboxPass;
	std::unique_ptr<GBufferPass> _gbufferPass;
	std::unique_ptr<DeferredLightingPass> _deferredLightingPass;
	std::unique_ptr<SSAOPass> _ssaoPass;
	std::unique_ptr<SSAOBlurPass> _ssaoBlurPass;

	IBLSystem _iblSystem;

	// frame rendering functions
	VkCommandBuffer begin_frame();
	void end_frame();

	// renderer initialization functions
	void create_command_buffers();
	void create_sync_structures();
	void create_render_targets();
	
	void blit_to_swapchain(VkCommandBuffer cmd);

	void extract_scene_data(const entt::registry& registry, DrawContext& ctx);
	void collect_draw_nodes(const std::shared_ptr<Node>& node, const glm::mat4& parentMatrix, DrawContext& ctx);

};