#pragma once 
#include <graphics/utils/vk_types.h>

class PipelineBuilder
{
public:
	std::vector<VkPipelineShaderStageCreateInfo> _shaderStages;

	VkPipelineInputAssemblyStateCreateInfo _inputAssembly;
	VkPipelineRasterizationStateCreateInfo _rasterizer;
	VkPipelineColorBlendAttachmentState _colorBlendAttachment;
	VkPipelineMultisampleStateCreateInfo _multisampling;
	VkPipelineLayout _pipelineLayout;
	VkPipelineDepthStencilStateCreateInfo _depthStencil;
	VkPipelineRenderingCreateInfo _renderInfo;
	VkFormat _colorAttachmentFormat;
	std::vector<VkDynamicState> _dynamicStates;

	PipelineBuilder() { clear(); }

	void clear();

	VkPipeline build_pipeline(VkDevice device);

	void set_dynamic_states(std::span<const VkDynamicState> states);

	void set_input_topology(VkPrimitiveTopology topology);

	void set_polygon_mode(VkPolygonMode mode, float lineWidth = 1.0f);

	void set_cull_mode(VkCullModeFlags cullMode);

	void set_front_face(VkFrontFace frontFace);

	void set_multisampling_none();

	void disable_blending();

	void enable_blending_additive();

	void enable_blending_alphablend();

	void set_depth_format(VkFormat format);

	void disable_depthtest();

	void enable_depthtest(bool depthWriteEnable, VkCompareOp op);

	// compute pipeline building
	VkPipelineShaderStageCreateInfo _computeShaderStage;

	VkPipeline build_compute_pipeline(VkDevice device);
};