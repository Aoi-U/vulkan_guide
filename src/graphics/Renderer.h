#pragma once
#include <core/vk_types.h>
#include <graphics/VulkanCore.h>
#include <graphics/Swapchain.h>

#include <graphics/pass/FrustumCullpass.h>
#include <graphics/pass/ShadowPass.h>
#include <graphics/pass/SkyboxPass.h>
#include <graphics/pass/PointShadowPass.h>
#include <graphics/pass/GBufferPass.h>
#include <graphics/pass/DeferredLightingPass.h>
#include <graphics/pass/SSAOPass.h>
#include <graphics/pass/SSAOBlurPass.h>

#include <resources/Buffer.h>
#include <graphics/IBLSystem.h>
#include <graphics/RenderResourceManager.h>
#include <window/window.h>
#include <utils/vk_descriptors.h>
#include <memory>
#include <entt.hpp>
#include <vector>
#include <core/Events.h>
#include <core/cvars.h>

extern AutoCVar_Float cvRenderScale;

extern AutoCVar_Int cvSSAO;
extern AutoCVar_Float cvSSAORadius;
extern AutoCVar_Float cvSSAOBias;
extern AutoCVar_Float cvSSAOPower;

class GuiRenderer;
class ResourceManager;

struct IndirectBatch
{
	VkBuffer indexBuffer;
	VkDescriptorSet materialSet;
	uint32_t first; // first index in the indirect buffer
	uint32_t count; // number of indirect commands in this batch
	uint32_t batchIndex; // index of this batch in the batch array
	bool isDoubleSided{ false };
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


	Buffer sceneDataBuffer; // uniform buffer for scene data

	// indirect draw buffers
	Buffer _cullStatsBuffer; // buffer for frustum cull statistics
	Buffer _gpuObjectBuffer; // buffer for object data used in frustum culling
	Buffer _gpuInstanceBuffer; // buffer for instance ids for visible objects
	Buffer _gpuIndirectBuffer; // buffer for indirect draw commands
	Buffer _gpuCountBuffer; // buffer for draw counts per batch
};

class Renderer
{
public:
	static constexpr unsigned int FRAME_OVERLAP = 2;

	void init(Window* window, VulkanCore* vulkanCore, entt::dispatcher& dispatcher, ResourceManager* resourceManager);

	void cleanup(entt::dispatcher& dispatcher);

	void on_window_resize(const Events::WindowResizeEvent& event);
	void set_gui_renderer(GuiRenderer* guiRenderer) { _guiRenderer = guiRenderer; }
	void set_viewport_extent(VkExtent2D extent);

	void render_frame(const entt::registry& registry, const GPUSceneData& sceneData);
	void recreate_swapchain();

	bool load_environment(const std::filesystem::path& path) { return _iblSystem.load_hdr(path); }
	void remove_environment() { _iblSystem.load_procedural_default(); }

	VkExtent2D get_swapchain_extent() const { return _swapchain.get_extent(); }
	VkExtent2D get_draw_extent() const { return _drawExtent; }
	VkImageView get_current_swapchain_image_view() const { return _swapchain.get_image_views()[_swapchainImageIndex]; }
	VkFormat get_swapchain_format() const { return _swapchain.get_image_format(); }
	FrameData& get_current_frame() { return _frames[_frameNumber % FRAME_OVERLAP]; }
	RenderResourceManager& get_resources() { return _renderResourceManager; }
	IBLSystem& get_ibl() { return _iblSystem; }
	GuiRenderer* get_gui_renderer() const { return _guiRenderer; }

	VkExtent2D get_viewport_extent() const { return _viewportExtent; }
	VkDescriptorSet get_scene_color_descriptor();
	VkDescriptorSet get_target_descriptor(const std::string& name, uint32_t faceIndex = 0);
	const std::vector<PassStats> get_stats() const;

private:
	Window* _window;
	VulkanCore* _vulkanCore;
	Swapchain _swapchain;
	RenderResourceManager _renderResourceManager;
	VkExtent2D _drawExtent;
	VkExtent2D _viewportExtent{ 1280, 720 };
	FrameData _frames[FRAME_OVERLAP];

	uint32_t _frameNumber{ 0 };
	uint32_t _swapchainImageIndex{ 0 };
	bool _isFrameStarted{ false };
	bool _resizeRequested{ false };
	VkExtent2D _pendingResizeExtent{ 0, 0 };
	GuiRenderer* _guiRenderer{ nullptr };
	float _currentRenderScale{ 1.0f };

	std::unique_ptr<FrustumCullPass> _frustumCullPass;
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

	void update_render_extent();
	
	void blit_to_swapchain(VkCommandBuffer cmd);

	void extract_scene_data(const entt::registry& registry, DrawContext& ctx);
	void collect_draw_nodes(const std::shared_ptr<Node>& node, const glm::mat4& parentMatrix, DrawContext& ctx);

};