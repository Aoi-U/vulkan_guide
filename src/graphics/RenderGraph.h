#pragma once

#include <graphics/utils/vk_types.h>
#include <graphics/RenderContext.h>
#include <graphics/utils/RGParameters.h>
#include <graphics/utils/RenderTargetPool.h>
#include <resources/Texture.h>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <optional>


struct RGTextureDesc
{
	VkExtent3D extent{ 1, 1, 1 };
	VkFormat format{ VK_FORMAT_UNDEFINED };
	VkImageUsageFlags usage{ 0 };
	uint32_t mipLevels{ 1 };
	uint32_t arrayLayers{ 1 };
	VkImageCreateFlags flags{ 0 };
	VkImageViewType viewType{ VK_IMAGE_VIEW_TYPE_2D };
	VkSampleCountFlagBits samples{ VK_SAMPLE_COUNT_1_BIT };
};

struct RGBufferDesc
{
	VkDeviceSize size{ 0 };
	VkBufferUsageFlags usage{ 0 };
};

struct RGTextureNode
{
	std::string name;
	RGTextureDesc desc;
	VkImage image{ VK_NULL_HANDLE };
	VkImageView imageView{ VK_NULL_HANDLE };
	std::vector<VkImageView> faceViews;
	VmaAllocation allocation{ VK_NULL_HANDLE };
	VkImageLayout initialLayout{ VK_IMAGE_LAYOUT_UNDEFINED };
	VkImageLayout currentLayout{ VK_IMAGE_LAYOUT_UNDEFINED };

	bool isExternal{ false }; // if true, texture is managed externally and not by render graph
	uint32_t firstPass{ UINT32_MAX };
	uint32_t lastPass{ 0 };
	uint32_t refCount{ 0 };
};

struct RGBufferNode
{
	std::string name;
	RGBufferDesc desc;
	VkBuffer buffer;
	VmaAllocation allocation{ VK_NULL_HANDLE };
	VkPipelineStageFlags2 currentStage{ VK_PIPELINE_STAGE_2_NONE };
	VkAccessFlags2 currentAccess{ VK_ACCESS_2_NONE };

	bool isExternal{ false }; // if true, buffer is managed externally and not by render graph
	uint32_t firstPass{ UINT32_MAX };
	uint32_t lastPass{ 0 };
	uint32_t refCount{ 0 };
};

struct RGResourceAccess
{
	RGResourceUsage usage;
	VkPipelineStageFlags2 stage;
	VkAccessFlags2 access;
	VkImageLayout layout{ VK_IMAGE_LAYOUT_UNDEFINED };
	VkAttachmentLoadOp loadOp{ VK_ATTACHMENT_LOAD_OP_DONT_CARE };
	VkClearColorValue clearColor{};
	float clearDepth{ 1.0f };
};


class RenderGraph;

class RGPassBuilder
{
public:
	RGPassBuilder(RenderGraph& graph, uint32_t passIndex)
		: _graph(graph), _passIndex(passIndex) {}

	RGTextureHandle read_texture(RGTextureHandle handle, VkPipelineStageFlags2 stage, VkImageLayout layout, VkAccessFlags2 access = VK_ACCESS_2_SHADER_READ_BIT);
	RGTextureHandle write_texture(RGTextureHandle handle, VkPipelineStageFlags2 stage, VkImageLayout layout, VkAccessFlags2 access = VK_ACCESS_2_SHADER_WRITE_BIT);

	RGTextureHandle set_color_attachment(RGTextureHandle handle, VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, VkClearColorValue clearColor = { 0.0f, 0.0f, 0.0f, 1.0f });
	RGTextureHandle set_depth_attachment(RGTextureHandle handle, VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, float clearDepth = 1.0f);

	RGBufferHandle read_buffer(RGBufferHandle handle, VkPipelineStageFlags2 stage, VkAccessFlags access);
	RGBufferHandle write_buffer(RGBufferHandle handle, VkPipelineStageFlags2 stage, VkAccessFlags access);

	void set_extent(VkExtent2D extent);
	void set_side_effect(bool hasSideEffect = true);

private:
	RenderGraph& _graph;
	uint32_t _passIndex;
};

struct IRGPass
{
	std::string name;
	uint32_t index{ 0 };
	bool culled{ false };
	bool hasSideEffect{ false };
	uint32_t refCount{ 0 };

	std::vector<std::pair<RGTextureHandle, RGResourceAccess>> textureAccesses;
	std::vector<std::pair<RGBufferHandle, RGResourceAccess>> bufferAccesses;
	std::optional<VkExtent2D> renderExtent;

	virtual ~IRGPass() = default;
	virtual void execute(RenderContext& ctx, RenderGraph& graph) = 0;
};


template<typename T>
struct RGPass : public IRGPass
{
	T data;
	std::function<void(const T& data, RenderContext& ctx)> executeFn;

	void execute(RenderContext& ctx, RenderGraph& graph) override // execute the pass
	{
		if (executeFn)
			executeFn(data, ctx);
	}
};

class RenderGraph
{
public:
	RenderGraph() = default;
	~RenderGraph() { reset(); }

	void init(RenderTargetPool* pool);
	void reset();

	RGTextureHandle create_texture(std::string_view name, const RGTextureDesc& desc);
	RGBufferHandle create_buffer(std::string_view name, const RGBufferDesc& desc);

	RGTextureHandle import_texture(std::string_view name, VkImage image, VkImageView view, VkFormat format, VkExtent3D extent, VkImageLayout currentLayout);
	RGTextureHandle import_texture(std::string_view name, const ResourceHandle<Texture>& tex, VkImageLayout currentLayout);
	RGBufferHandle  import_buffer(std::string_view name, VkBuffer buffer, VkDeviceSize size, VkPipelineStageFlags2 stage, VkAccessFlags2 access);
	void set_texture_layout(RGTextureHandle handle, VkImageLayout layout);

	RGTextureHandle find_texture(std::string_view name) const;
	RGBufferHandle  find_buffer(std::string_view name) const;

	template<typename T, typename SetupFunc, typename ExecuteFunc>
	const T& add_pass(std::string_view name, SetupFunc&& setupFn, ExecuteFunc&& executeFn)
	{
		auto pass = std::make_unique<RGPass<T>>();
		pass->name = std::string(name);
		pass->index = static_cast<uint32_t>(_passes.size());
		pass->executeFn = std::forward<ExecuteFunc>(executeFn);

		_passes.push_back(std::move(pass));

		RGPassBuilder builder(*this, static_cast<uint32_t>(_passes.size() - 1));
		setupFn(builder, static_cast<RGPass<T>*>(_passes.back().get())->data);

		return static_cast<RGPass<T>*>(_passes.back().get())->data;
	}

	void compile(uint32_t currentFrame);
	void execute(RenderContext& ctx);
	
	// getters
	VkImage get_image(RGTextureHandle handle) const;
	VkImageView get_image_view(RGTextureHandle handle, uint32_t faceIndex = UINT32_MAX) const;
	VkFormat get_format(RGTextureHandle handle) const;
	VkExtent3D get_extent(RGTextureHandle handle) const;
	bool is_cube(RGTextureHandle handle) const;
	VkBuffer get_buffer(RGBufferHandle handle) const;

	// for side effect passes
	void transition_texture(VkCommandBuffer cmd, RGTextureHandle handle, VkImageLayout newLayout);

private:
	friend class RGPassBuilder;

	RenderTargetPool* _renderTargetPool{ nullptr };

	void cull_passes();
	void calculate_lifetimes();
	void allocate_target_resources(uint32_t currentFrame);
	void dispatch_barriers(RenderContext& ctx, IRGPass* pass);

	std::vector<std::unique_ptr<IRGPass>> _passes;
	std::vector<RGTextureNode> _textures;
	std::vector<RGBufferNode> _buffers;
};