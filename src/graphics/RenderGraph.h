#pragma once

#include <core/vk_types.h>
#include <graphics/RenderContext.h>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <optional>


struct RGTextureState
{
	VkImage image{ VK_NULL_HANDLE };
	VkFormat format{ VK_FORMAT_UNDEFINED };
	VkImageLayout currentLayout{ VK_IMAGE_LAYOUT_UNDEFINED };
	uint32_t layerCount{ 1 };
	uint32_t mipLevels{ 0 };
	uint32_t baseMiplevel{ 0 };
};

struct RGBufferState
{
	VkBuffer buffer{ VK_NULL_HANDLE };
	VkPipelineStageFlags2 currentStage{ VK_PIPELINE_STAGE_2_NONE };
};

struct RGTextureTransition
{
	std::string name;
	VkImageLayout targetLayout;
};

struct RGBufferTransition
{
	std::string name;
	VkPipelineStageFlags2 targetStage;
};

struct RGColorAttachment
{
	std::string name;
	VkImageView view;
	VkAttachmentLoadOp loadOp;
	VkAttachmentStoreOp storeOp;
	VkClearColorValue clearColor;
};

struct RGDepthAttachment
{
	std::string name;
	VkImageView view;
	VkAttachmentLoadOp loadOp;
	VkAttachmentStoreOp storeOp;
	float clearDepth;
};

using ExecutionFn = std::function<void(RenderContext& ctx)>;

struct RGPass
{
	std::string name;
	std::vector<RGTextureTransition> textureTransitions;
	std::vector<RGBufferTransition> bufferTransitions;

	std::vector<RGColorAttachment> colorAttachments;
	std::optional<RGDepthAttachment> depthAttachment;
	std::optional<VkExtent2D> renderExtent;

	ExecutionFn executeCallback;
};

class RenderGraph;

class RGPassBuilder
{
public:
	RGPassBuilder(RenderGraph& graph, RGPass& pass)
		: _graph(graph), _pass(pass) {}

	RGPassBuilder& read_texture(const std::string& name, VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	RGPassBuilder& write_texture(const std::string& name, VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL);

	RGPassBuilder& read_buffer(const std::string& name, VkPipelineStageFlags2 stage);
	RGPassBuilder& write_buffer(const std::string& name, VkPipelineStageFlags2 stage);
	
	RGPassBuilder& set_color_attachment(const std::string& name, VkImageView view, VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, const VkClearColorValue& clearColor = { 0.0f, 0.0f, 0.0f, 1.0f });
	RGPassBuilder& set_depth_attachment(const std::string& name, VkImageView view, VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, float clearDepth = 1.0f);

	RGPassBuilder& set_extent(VkExtent2D extent);
	RGPassBuilder& set_execute_callback(ExecutionFn function);

private:
	RenderGraph& _graph;
	RGPass& _pass;
};

class RenderGraph
{
public:
	void clear_passes();

	void register_texture(const std::string& name, VkImage image, VkFormat format, VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, uint32_t layerCount = 1, uint32_t mipLevels = 1, uint32_t baseMiplevel = 0);
	void register_buffer(const std::string& name, VkBuffer buffer, VkPipelineStageFlags2 initialStage = VK_PIPELINE_STAGE_2_NONE);

	RGPassBuilder add_pass(const std::string name);

	void execute(RenderContext& ctx);

	// immediate transitions
	void transition_texture(VkCommandBuffer cmd, const std::string& name, VkImageLayout newLayout);
	void transition_buffer(VkCommandBuffer cmd, const std::string& name, VkPipelineStageFlags2 newStage);

	RGTextureState* get_texture(const std::string& name);
	RGBufferState* get_buffer(const std::string& name);

private:
	std::unordered_map<std::string, RGTextureState> _textures;
	std::unordered_map<std::string, RGBufferState> _buffers;
	std::vector<RGPass> _passes;
};