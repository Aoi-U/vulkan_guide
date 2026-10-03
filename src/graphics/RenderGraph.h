#pragma once

#include <core/vk_types.h>
#include <graphics/RenderContext.h>
#include <graphics/RenderResourceManager.h>
#include <graphics/utils/RenderSync.h>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <optional>


struct RGColorAttachment
{
	std::string name;
	VkAttachmentLoadOp loadOp{ VK_ATTACHMENT_LOAD_OP_CLEAR };
	VkAttachmentStoreOp storeOp{ VK_ATTACHMENT_STORE_OP_STORE };
	VkClearColorValue clearColor{ 0.0f, 0.0f, 0.0f, 1.0f };
};

struct RGDepthAttachment
{
	std::string name;
	VkAttachmentLoadOp loadOp{ VK_ATTACHMENT_LOAD_OP_CLEAR };
	VkAttachmentStoreOp storeOp{ VK_ATTACHMENT_STORE_OP_STORE };
	float clearDepth{ 0.0f };
};

struct RGResourceUsage
{
	std::string name;
	ResourceState state;
};

using ExecutionFn = std::function<void(RenderContext& ctx)>;

struct RGPass
{
	std::string name;
	std::vector<RGResourceUsage> reads;
	std::vector<RGResourceUsage> writes;
	std::vector<RGColorAttachment> colorAttachments;
	std::optional<RGDepthAttachment> depthAttachments;
	std::optional<VkExtent2D> renderExtent;
	ExecutionFn executeCallback;
};

class RGPassBuilder
{
public:
	explicit RGPassBuilder(RGPass& pass) : _pass(pass) {}

	// resource this pass reads from
	RGPassBuilder& read(const std::string& name, ResourceState state = ResourceState::ShaderReadOnly)
	{
		_pass.reads.push_back({ name, state });
		return *this;
	}

	// resource this pass writes to
	RGPassBuilder& write(const std::string& name, ResourceState state = ResourceState::StorageImage)
	{
		_pass.writes.push_back({ name, state });
		return *this;
	}

	// set color attachment for this pass
	RGPassBuilder& set_color_attachment(const std::string& name, VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, VkClearColorValue clearColor = { 0.0f, 0.0f, 0.0f, 1.0f })
	{
		_pass.writes.push_back({ name, ResourceState::ColorAttachment });
		_pass.colorAttachments.push_back({ name, loadOp, VK_ATTACHMENT_STORE_OP_STORE, clearColor });
		return *this;
	}

	// set depth attachment for this pass
	RGPassBuilder& set_depth_attachment(const std::string& name, VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, float clearDepth = 0.0f)
	{
		_pass.writes.push_back({ name, ResourceState::DepthAttachment });
		_pass.depthAttachments = RGDepthAttachment{ name, loadOp, VK_ATTACHMENT_STORE_OP_STORE, clearDepth };
		return *this;
	}

	// set the render extent for this pass
	RGPassBuilder& set_extent(VkExtent2D extent)
	{
		_pass.renderExtent = extent;
		return *this;
	}

	// set the execute callback for this pass
	RGPassBuilder& set_execute_callback(ExecutionFn fn)
	{
		_pass.executeCallback = std::move(fn);
		return *this;
	}

private:
	RGPass& _pass;
};

class RenderGraph
{
public:
	explicit RenderGraph(RenderResourceManager& resources) : _resources(resources) {}

	// add a new pass to the graph
	RGPassBuilder add_pass(const std::string& name);

	// compile the graph to determine execution order and resource transitions
	void compile();

	// execute the graph by running each pass in order
	void execute(RenderContext& ctx);

private:
	RenderResourceManager& _resources;
	std::vector<RGPass> _passes;
	std::vector<uint32_t> _executionOrder;
};