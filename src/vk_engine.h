// vulkan_guide.h : Include file for standard system include files,
// or project specific include files.

#pragma once

#include <utils/vk_types.h>
#include <utils/vk_loader.h>
#include <utils/vk_descriptors.h>

#include <SwapchainManager.h>
#include <span>

struct ComputePushConstants
{
	glm::vec4 data1;
	glm::vec4 data2;
	glm::vec4 data3;
	glm::vec4 data4;
};

struct ComputeEffect
{
	const char* name;

	VkPipeline pipeline;
	VkPipelineLayout layout;

	ComputePushConstants data;
};

// NOTE: inefficient at scale. better implementation would be to store arrays of vulkan handles of various types (VkImage, VkBuffer, etc)
// and delete them from a loop
struct DeletionQueue
{
	std::deque<std::function<void()>> deletors;

	// adds a function to the deletion queue
	void push_function(std::function<void()>&& function)
	{
		deletors.push_back(function);
	}

	// flushes the deletion queue
	void flush()
	{
		// reverse iterate the deletion queue to execute all functions
		for (auto it = deletors.rbegin(); it != deletors.rend(); it++) {
			(*it)(); // call the function
		}

		deletors.clear();
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
};

struct GPUSceneData
{
	glm::mat4 view;
	glm::mat4 proj;
	glm::mat4 viewproj;
	glm::vec4 ambientCOlor;
	glm::vec4 sunlightDirection; // w component for sun power
	glm::vec4 sunlightColor;
};

constexpr unsigned int FRAME_OVERLAP = 2;

class VulkanEngine 
{
public:

	bool _isInitialized{ false };
	int _frameNumber{ 0 };
	bool stop_rendering{ false };
	VkExtent2D _windowExtent{ 1700 , 900 };

	// vulkan instance handles
	VkInstance _instance; // vulkan library handle
	VkDebugUtilsMessengerEXT _debug_messenger; // vulkan debug output handle
	VkPhysicalDevice _chosenGPU; // gpu chosen as the default device
	VkDevice _device; // vulkan logical device for comands
	VkSurfaceKHR _surface; // vulkan window surface

	// vulkan swapchain handle
	SwapchainManager _swapchain;
	bool resize_requested{ false };

	FrameData _frames[FRAME_OVERLAP];
	FrameData& get_current_frame() { return _frames[_frameNumber % FRAME_OVERLAP]; }

	// draw resources
	AllocatedImage _drawImage;
	AllocatedImage _depthImage;
	VkExtent2D _drawExtent;
	float renderScale{ 1.0f };

	VkQueue _graphicsQueue;
	uint32_t _graphicsQueueFamily;

	VmaAllocator _allocator;

	DescriptorAllocator _globalDescriptorAllocator;
	VkDescriptorSet _drawImageDescriptorSet;
	VkDescriptorSetLayout _drawImageDescriptorLayout;

	GPUSceneData sceneData;
	VkDescriptorSetLayout _gpuSceneDataDescriptorLayout;

	VkPipeline _gradientPipeline;
	VkPipelineLayout _gradientPipelineLayout;

	VkPipeline _meshPipeline;
	VkPipelineLayout _meshPipelineLayout;

	std::vector<std::shared_ptr<MeshAsset>> testMeshes;

	DeletionQueue _mainDeletionQueue;

	// immediate submit structures
	VkFence _immFence;
	VkCommandBuffer _immCommandBuffer;
	VkCommandPool _immCommandPool;

	std::vector<ComputeEffect> backgroundEffects;
	int currentBackgroundEffect{ 0 };

	struct SDL_Window* _window{ nullptr };

	static VulkanEngine& Get();

	//initializes everything in the engine
	void init();

	//shuts down the engine
	void cleanup();

	//draw loop
	void draw();

	void draw_background(VkCommandBuffer cmd);
	void draw_geometry(VkCommandBuffer cmd);
	void draw_imgui(VkCommandBuffer cmd, VkImageView targetImageView);

	void immediate_submit(std::function<void(VkCommandBuffer cmd)>&& function);

	//run main loop
	void run();

	AllocatedBuffer create_buffer(size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage);
	void destroy_buffer(const AllocatedBuffer& buffer);

	GPUMeshBuffers uploadMesh(std::span<uint32_t> indices, std::span<Vertex> vertices);

	void init_default_data();

private:

	// vulkan initialization functions
	void init_vulkan();
	void init_swapchain();
	void init_commands();
	void init_sync_structures();
	void init_descriptors();
	void init_pipelines();
	void init_background_pipelines();
	void init_mesh_pipeline();
	void init_imgui();


	void resize_swapchain();
};
