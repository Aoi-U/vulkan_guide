#pragma once
#include <core/vk_types.h>
#include <graphics/VulkanCore.h>
#include <graphics/Swapchain.h>
#include <graphics/pass/RenderPass.h>
#include <graphics/pass/ComputePass.h>
#include <graphics/MaterialSystem.h>
#include <window/window.h>
#include <utils/vk_descriptors.h>
#include <memory>
#include <entt.hpp>
#include <vector>

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

class Renderer
{
public:
	static constexpr unsigned int FRAME_OVERLAP = 2;

	void init(Window* window, VulkanCore* vulkanCore, MaterialSystem* materialSystem);

	void cleanup();

	using GuiDrawFunction = std::function<void(VkCommandBuffer cmd, VkImageView targetImageView, VkExtent2D drawExtent)>;
	//void render_scene(const entt::registry& registry, const GPUSceneData& sceneData, GuiDrawFunction guiDrawFunction = nullptr);

	void render_frame(const entt::registry& registry, const GPUSceneData& sceneData, GuiDrawFunction guiDrawFunction = nullptr);

	void recreate_swapchain();

	VkExtent2D get_swapchain_extent() const { return _swapchain.get_extent(); }
	VkExtent2D get_draw_extent() const { return _drawExtent; }
	VkImageView get_current_swapchain_image_view() const {
		return _swapchain.get_image_views()[_swapchainImageIndex];
	}
	VkFormat get_swapchain_format() const { return _swapchain.get_image_format(); }
	AllocatedImage get_draw_image() const { return _drawImage; }
	AllocatedImage get_depth_image() const { return _depthImage; }
	FrameData& get_current_frame() { return _frames[_frameNumber % FRAME_OVERLAP]; }
	const std::vector<PassStats> get_stats() const;

private:
	Window* _window;
	VulkanCore* _vulkanCore;
	Swapchain _swapchain;
	
	AllocatedImage _drawImage;
	AllocatedImage _depthImage;
	VkExtent2D _drawExtent;

	FrameData _frames[FRAME_OVERLAP];
	uint32_t _frameNumber{ 0 };
	uint32_t _swapchainImageIndex{ 0 };
	bool _isFrameStarted{ false };

	//std::unique_ptr<MeshRenderer> _meshRenderer;
	std::vector<std::unique_ptr<RenderPass>> _renderPasses;
	std::vector<std::unique_ptr<ComputePass>> _computePasses;

	// renderer initialization functions
	void create_command_buffers();
	void create_sync_structures();
	void create_render_targets();
	void destroy_render_targets();
	
	// frame rendering functions
	VkCommandBuffer begin_frame();
	void begin_rendering(VkCommandBuffer cmd);
	void end_rendering(VkCommandBuffer cmd);
	void end_frame();

	void blit_to_swapchain(VkCommandBuffer cmd);

	void extract_scene_data(const entt::registry& registry, DrawContext& ctx);
	void collect_draw_nodes(const std::shared_ptr<Node>& node, const glm::mat4& parentMatrix, DrawContext& ctx);

};