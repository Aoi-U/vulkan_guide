#pragma once

#include <graphics/utils/vk_types.h>
#include <resources/Shader.h>
#include <graphics/reflection/ShaderReflectionTypes.h>
#include <vector>
#include <memory>
#include <span>

class ResourceManager;

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
	std::vector<VkFormat> colorAttachmentFormats;
	VkFormat depthAttachmentFormat{ VK_FORMAT_UNDEFINED };
	DepthState depthState;
	RasterState rasterState;
	BlendMode blendMode{ BlendMode::None };
	VkPrimitiveTopology topology{ VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	std::vector<VkDynamicState> dynamicStates{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE };
};

struct ComputePipelineDesc
{
	ResourceHandle<Shader> computeShader;
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
	GraphicsPipeline(VkDevice device, ResourceManager* resourceManager, const GraphicsPipelineDesc& desc);
	GraphicsPipeline(GraphicsPipeline&&) noexcept = default;
	GraphicsPipeline& operator=(GraphicsPipeline&&) noexcept = default;

	const PipelineLayoutReflection& get_reflection() const { return _reflection; }
	VkDescriptorSetLayout get_descriptor_set_layout(uint32_t set) const
	{
		auto it = _setLayouts.find(set);
		return (it != _setLayouts.end()) ? it->second : VK_NULL_HANDLE;
	}

private:
	PipelineLayoutReflection _reflection;
	std::unordered_map<uint32_t, VkDescriptorSetLayout> _setLayouts;
};


class ComputePipeline : public Pipeline
{
public:
	ComputePipeline() = default;
	ComputePipeline(VkDevice device, ResourceManager* resourceManager, const ComputePipelineDesc& desc);
	ComputePipeline(ComputePipeline&&) noexcept = default;
	ComputePipeline& operator=(ComputePipeline&&) noexcept = default;

	const PipelineLayoutReflection& get_reflection() const { return _reflection; }
	VkDescriptorSetLayout get_descriptor_set_layout(uint32_t set) const
	{
		auto it = _setLayouts.find(set);
		return (it != _setLayouts.end()) ? it->second : VK_NULL_HANDLE;
	}

private:
	PipelineLayoutReflection _reflection;
	std::unordered_map<uint32_t, VkDescriptorSetLayout> _setLayouts;
};