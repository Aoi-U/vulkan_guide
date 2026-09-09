// vulkan_guide.h : Include file for standard system include files,
// or project specific include files.

#pragma once

#include <utils/vk_types.h>
#include <utils/vk_loader.h>
#include <utils/vk_descriptors.h>

#include <SwapchainManager.h>
#include <span>
#include <camera.h>

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

struct GLTFMetallic_Roughness
{
	MaterialPipeline opaquePipeline;
	MaterialPipeline transparentPipeline;

	VkDescriptorSetLayout materialLayout;

	struct MaterialConstants
	{
		glm::vec4 colorFactors;
		glm::vec4 metalRoughFactors;
		// padding, need it anyway for uniform bnuffers
		glm::vec4 padding[14];
	};

	struct MaterialResources
	{
		AllocatedImage colorImage;
		VkSampler colorSampler;
		AllocatedImage metalRoughImage;
		VkSampler metalRoughSampler;
		VkBuffer dataBuffer;
		uint32_t dataBufferOffset;
	};

	DescriptorWriter writer;

	void build_pipelines(VulkanEngine* engine);
	void clear_resources(VkDevice device);

	MaterialInstance write_material(VkDevice device, MaterialPass pass, const MaterialResources& resources, DescriptorAllocatorGrowable& descriptorAllocator);
};

// core render object data 
struct RenderObject
{
	uint32_t indexCount;
	uint32_t firstIndex;
	VkBuffer indexBuffer;

	MaterialInstance* material;

	glm::mat4 transform;
	VkDeviceAddress vertexBufferAddress;
};

struct DrawContext
{
	std::vector<RenderObject> opaqueSurfaces;
};

// scene graph node for a renderable mesh
struct MeshNode : public Node {
	std::shared_ptr<MeshAsset> mesh;

	virtual void draw(const glm::mat4& topMatrix, DrawContext& ctx) override;
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

	DescriptorAllocatorGrowable _globalDescriptorAllocator;

	VkDescriptorSet _drawImageDescriptorSet;
	VkDescriptorSetLayout _drawImageDescriptorLayout;
	VkDescriptorSetLayout _singleImageDescriptorLayout;

	GPUSceneData sceneData;
	VkDescriptorSetLayout _gpuSceneDataDescriptorLayout;

	VkPipeline _gradientPipeline;
	VkPipelineLayout _gradientPipelineLayout;

	VkPipeline _meshPipeline;
	VkPipelineLayout _meshPipelineLayout;

	GPUMeshBuffers rectangle;
	std::vector<std::shared_ptr<MeshAsset>> testMeshes;

	DrawContext mainDrawContext;
	std::unordered_map<std::string, std::shared_ptr<Node>> loadedNodes;

	AllocatedImage _whiteImage;
	AllocatedImage _blackImage;
	AllocatedImage _greyImage;
	AllocatedImage _errorCheckerboardImage;

	MaterialInstance defaultData;
	GLTFMetallic_Roughness metalRoughMaterial;

	VkSampler _defaultSamplerLinear;
	VkSampler _defaultSamplerNearest;

	DeletionQueue _mainDeletionQueue;

	// immediate submit structures
	VkFence _immFence;
	VkCommandBuffer _immCommandBuffer;
	VkCommandPool _immCommandPool;

	std::vector<ComputeEffect> backgroundEffects;
	int currentBackgroundEffect{ 0 };

	struct SDL_Window* _window{ nullptr };

	Camera mainCamera; // NOTE: move camera to gameplay layer. engine should just have the matrices needed for rendering

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

	void update_scene();

	void immediate_submit(std::function<void(VkCommandBuffer cmd)>&& function);

	//run main loop
	void run();

	AllocatedBuffer create_buffer(size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage);
	void destroy_buffer(const AllocatedBuffer& buffer);

	AllocatedImage create_image(VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped = false);
	AllocatedImage create_image(void* data, VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped = false);
	void destroy_image(const AllocatedImage& image);

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
