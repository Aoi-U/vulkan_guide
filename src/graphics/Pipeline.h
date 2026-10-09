#pragma once

#include <graphics/utils/vk_types.h>
#include <resources/Shader.h>
#include <vector>
#include <memory>
#include <span>

enum class BlendMode
{
	None,
	Additive,
	AlphaBlend
};

struct DepthState
{
	bool depthTest{ true };
	bool depthWrite{ true };
	VkCompareOp compareOp{ VK_COMPARE_OP_GREATER_OR_EQUAL };
};

struct RasterState
{
	VkCullModeFlags cullMode{ VK_CULL_MODE_BACK_BIT };
	VkFrontFace frontFace{ VK_FRONT_FACE_CLOCKWISE };
	VkPolygonMode polygonMode{ VK_POLYGON_MODE_FILL };
};

struct GraphicsPipelineDesc
{
	std::vector<ResourceHandle<Shader>> shaders;
	std::vector<VkDescriptorSetLayout> descriptorLayouts;
	std::vector<VkPushConstantRange> pushConstants;
	std::vector<VkFormat> colorAttachmentFormats;
	VkFormat depthAttachmentFormat{ VK_FORMAT_UNDEFINED };
	DepthState depthState;
	RasterState rasterState;
	BlendMode blendMode{ BlendMode::None };
	VkPrimitiveTopology topology{ VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	std::vector<VkDynamicState> dynamicStates{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
};

struct ComputePipelineDesc
{
	ResourceHandle<Shader> computeShader;
	std::vector<VkDescriptorSetLayout> descriptorLayouts;
	std::vector<VkPushConstantRange> pushConstants;
};

class Pipeline
{
public:
	Pipeline() = default;
	Pipeline(VkDevice device) : _device(device) {}
	~Pipeline() { cleanup(); }
	void cleanup();

	Pipeline(const Pipeline&) = delete;
	Pipeline& operator=(const Pipeline&) = delete;
	Pipeline(Pipeline&& other) noexcept;
	Pipeline& operator=(Pipeline&& other) noexcept;

	// getters
	VkPipeline get_pipeline() const { return _pipeline; }
	VkPipelineLayout get_layout() const { return _layout; }
	bool is_valid() const { return _pipeline != VK_NULL_HANDLE; }

protected:
	VkDevice _device{ VK_NULL_HANDLE };
	VkPipeline _pipeline{ VK_NULL_HANDLE };
	VkPipelineLayout _layout{ VK_NULL_HANDLE };
};

class GraphicsPipeline : public Pipeline
{
public:
	GraphicsPipeline() = default;
	GraphicsPipeline(VkDevice device, const GraphicsPipelineDesc& desc);
	GraphicsPipeline(GraphicsPipeline&&) noexcept = default;
	GraphicsPipeline& operator=(GraphicsPipeline&&) noexcept = default;
};


class ComputePipeline : public Pipeline
{
public:
	ComputePipeline() = default;
	ComputePipeline(VkDevice device, const ComputePipelineDesc& desc);
	ComputePipeline(ComputePipeline&&) noexcept = default;
	ComputePipeline& operator=(ComputePipeline&&) noexcept = default;
};