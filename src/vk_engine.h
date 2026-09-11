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

// represents a gltf material with metallic roughness 
struct GLTFMetallic_Roughness
{
	MaterialPipeline opaquePipeline;
	MaterialPipeline transparentPipeline;

	VkDescriptorSetLayout materialLayout;

	// data that is sent to shader for a material instance
	struct MaterialConstants
	{
		glm::vec4 colorFactors;
		glm::vec4 metalRoughFactors;
		// padding, need it anyway for uniform bnuffers
		glm::vec4 padding[14];
	};

	// resources needed for a material instance
	struct MaterialResources
	{
		AllocatedImage colorImage;
		VkSampler colorSampler;
		AllocatedImage metalRoughImage;
		VkSampler metalRoughSampler;
		VkBuffer dataBuffer;
		uint32_t dataBufferOffset;
	};

	// helper class to write descriptor sets for the material
	DescriptorWriter writer;

	// builds the pipelines for the material
	void build_pipelines(VulkanEngine* engine);
	void clear_resources(VkDevice device);

	// creates a material instance for a given pass and resources
	MaterialInstance write_material(VkDevice device, MaterialPass pass, const MaterialResources& resources, DescriptorAllocatorGrowable& descriptorAllocator);
};

// represents a single renderable object in the scene
struct RenderObject
{
	uint32_t indexCount;
	uint32_t firstIndex;
	VkBuffer indexBuffer;

	MaterialInstance* material;
	Bounds bounds;
	glm::mat4 transform;
	VkDeviceAddress vertexBufferAddress;
};

// context for a single draw call, contains all the render objects to be drawn
struct DrawContext
{
	std::vector<RenderObject> opaqueSurfaces;
	std::vector<RenderObject> transparentSurfaces;
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

struct EngineStats
{
	float frameTime;
	float deltaTime;
	int triangleCount;
	int drawcallCount;
	float sceneUpdateTime;
	float meshDrawTime;
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

	GPUSceneData sceneData;
	VkDescriptorSetLayout _gpuSceneDataDescriptorLayout;

	VkPipeline _gradientPipeline;
	VkPipelineLayout _gradientPipelineLayout;

	VkPipeline _meshPipeline;
	VkPipelineLayout _meshPipelineLayout;

	DrawContext mainDrawContext;
	std::unordered_map<std::string, std::shared_ptr<Node>> loadedNodes;
	std::unordered_map<std::string, std::shared_ptr<LoadedGLTF>> loadedScenes;

	// default textures
	AllocatedImage _whiteImage;
	AllocatedImage _blackImage;
	AllocatedImage _greyImage;
	AllocatedImage _errorCheckerboardImage;

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

	EngineStats stats;

	struct SDL_Window* _window{ nullptr };

	Camera mainCamera; // NOTE: move camera to gameplay layer. engine should just have the matrices needed for rendering

	static VulkanEngine& Get();

	//initializes everything in the engine
	void init();

	//shuts down the engine
	void cleanup();

	//draw loop
	void draw();

	// draws the background effect to the draw image
	void draw_background(VkCommandBuffer cmd);

	// draws the scene to the draw image
	void draw_geometry(VkCommandBuffer cmd);

	// draw imgui
	void draw_imgui(VkCommandBuffer cmd, VkImageView targetImageView);

	// updates the imgui interface
	void update_imgui();

	// updates scene data and push drawable objects to the draw context
	void update_scene();
	
	// submits a function to be executed immediately on the gpu
	void immediate_submit(std::function<void(VkCommandBuffer cmd)>&& function);

	//run main loop
	void run();

	// creates a buffer 
	AllocatedBuffer create_buffer(size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage);
	void destroy_buffer(const AllocatedBuffer& buffer);

	// creates an image
	AllocatedImage create_image(VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped = false);

	// creates an image with data
	AllocatedImage create_image(void* data, VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped = false);
	void destroy_image(const AllocatedImage& image);

	// uploads a mesh to the gpu and returns the buffers for it
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
	void init_imgui();
	void resize_swapchain();
};
