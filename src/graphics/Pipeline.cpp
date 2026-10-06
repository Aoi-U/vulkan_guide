#include "graphics/Pipeline.h"
#include <utils/vk_initializers.h>
#include <utils/vk_pipelines.h>

void Pipeline::cleanup()
{
	if (_pipeline != VK_NULL_HANDLE && _device != VK_NULL_HANDLE) {
		vkDestroyPipeline(_device, _pipeline, nullptr);
		_pipeline = VK_NULL_HANDLE;
	}

	if (_layout != VK_NULL_HANDLE && _device != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(_device, _layout, nullptr);
		_layout = VK_NULL_HANDLE;
	}
}

Pipeline::Pipeline(Pipeline&& other) noexcept
	: _device(other._device), _pipeline(other._pipeline), _layout(other._layout)
{
	other._device = VK_NULL_HANDLE;
	other._pipeline = VK_NULL_HANDLE;
	other._layout = VK_NULL_HANDLE;
}

Pipeline& Pipeline::operator=(Pipeline && other) noexcept
{
	if (this != &other) {
		cleanup();
		_device = other._device;
		_pipeline = other._pipeline;
		_layout = other._layout;
		other._device = VK_NULL_HANDLE;
		other._pipeline = VK_NULL_HANDLE;
		other._layout = VK_NULL_HANDLE;
	}
	return *this;
}

GraphicsPipeline::GraphicsPipeline(VkDevice device, const GraphicsPipelineDesc& desc)
	: Pipeline(device)
{
	// build layout
	VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipeline_layout_create_info();
	layoutInfo.setLayoutCount = static_cast<uint32_t>(desc.descriptorLayouts.size());
	layoutInfo.pSetLayouts = desc.descriptorLayouts.data(); 
	layoutInfo.pushConstantRangeCount = static_cast<uint32_t>(desc.pushConstants.size());
	layoutInfo.pPushConstantRanges = desc.pushConstants.data();
	VK_CHECK(vkCreatePipelineLayout(_device, &layoutInfo, nullptr, &_layout));

	// build pipeline
	PipelineBuilder builder;
	builder._pipelineLayout = _layout;

	for (const auto& shader : desc.shaders) {
		if (shader) {
			builder._shaderStages.push_back(shader->get_stage_info());
		}
	}
	
	builder.set_input_topology(desc.topology);
	builder.set_polygon_mode(desc.rasterState.polygonMode);
	builder.set_cull_mode(desc.rasterState.cullMode);
	builder.set_multisampling_none();

	switch (desc.blendMode) {
	case BlendMode::None:
		builder.disable_blending(); 
		break;
	case BlendMode::Additive:
		builder.enable_blending_additive();
		break;
	case BlendMode::AlphaBlend:
		builder.enable_blending_alphablend();
		break;
	}

	if (desc.depthState.depthTest) {
		builder.enable_depthtest(desc.depthState.depthWrite, desc.depthState.compareOp);
	}
	else {
		builder.disable_depthtest();
	}

	builder.set_depth_format(desc.depthAttachmentFormat);
	builder._renderInfo.colorAttachmentCount = static_cast<uint32_t>(desc.colorAttachmentFormats.size());
	builder._renderInfo.pColorAttachmentFormats = desc.colorAttachmentFormats.data();

	builder.set_dynamic_states(desc.dynamicStates);

	_pipeline = builder.build_pipeline(_device);
}


ComputePipeline::ComputePipeline(VkDevice device, const ComputePipelineDesc& desc)
	: Pipeline(device)
{
	// build layout
	VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipeline_layout_create_info();
	layoutInfo.setLayoutCount = static_cast<uint32_t>(desc.descriptorLayouts.size());
	layoutInfo.pSetLayouts = desc.descriptorLayouts.data();
	layoutInfo.pushConstantRangeCount = static_cast<uint32_t>(desc.pushConstants.size());
	layoutInfo.pPushConstantRanges = desc.pushConstants.data();
	VK_CHECK(vkCreatePipelineLayout(_device, &layoutInfo, nullptr, &_layout));

	// build pipeline
	PipelineBuilder builder;
	builder._pipelineLayout = _layout;
	if (desc.computeShader.is_valid()) {
		builder._computeShaderStage = desc.computeShader->get_stage_info();
	}

	_pipeline = builder.build_compute_pipeline(_device);
}
