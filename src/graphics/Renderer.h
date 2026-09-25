#pragma once
#include <graphics/VulkanCore.h>
#include <graphics/Swapchain.h>
#include <graphics/pass/RenderPass.h>
#include <graphics/pass/ForwardPass.h>
#include <window/window.h>
#include <utils/vk_descriptors.h>
#include <memory>
#include <entt.hpp>
#include <vector>


struct FrameData
{
	VkCommandPool _commandPool;
	VkCommandBuffer _mainCommandBuffer;

	VkSemaphore _swapchainSemaphore;
	VkFence _renderFence;

	DeletionQueue _deletionQueue;
	DescriptorAllocatorGrowable _frameDescriptors;
};

class Renderer
{
public:
	static constexpr unsigned int FRAME_OVERLAP = 2;

	void init(Window* window, VulkanCore* vulkanCore);

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
	GLTFMetallic_Roughness* get_material_system() { return _forwardPass->get_material_system(); }
	const std::vector<PassStats> get_stats() const;

	float renderScale{ 1.0f };

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
	ForwardPass* _forwardPass;
	std::vector<std::unique_ptr<RenderPass>> _renderPasses;

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