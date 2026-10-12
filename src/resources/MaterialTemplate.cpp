#include "MaterialTemplate.h"

#include <graphics/reflection/ShaderReflection.h>
#include <graphics/utils/vk_pipelines.h>
#include <graphics/VulkanCore.h>
#include <fmt/core.h>

MaterialTemplate::MaterialTemplate(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, ResourceHandle<Shader> vertShader, ResourceHandle<Shader> fragShader)
 : Resource(id, core, resourceManager), _vertShader(vertShader), _fragShader(fragShader) {}


VkPipeline MaterialTemplate::get_pipeline(const MaterialPipelineKey& key)
{
	auto it = _pipelineCache.find(key);
	if (it != _pipelineCache.end()) { // return cached pipeline
		return it->second;
	}

	// build new pipeline
	PipelineBuilder builder;
	builder._pipelineLayout = _pipelineLayout;
	builder._shaderStages.push_back(_vertShader->get_stage_info());
	builder._shaderStages.push_back(_fragShader->get_stage_info());

	builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	builder.set_polygon_mode(key.polygonMode);
	builder.set_cull_mode(key.cullMode);
	builder.set_front_face(key.frontFace);
	builder.set_multisampling_none();

	switch (key.blendMode) {
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

	if (key.depthTest) {
		builder.enable_depthtest(key.depthWrite, key.depthComareOp);
	}
	else {
		builder.disable_depthtest();
	}

	builder.set_depth_format(key.depthAttachmentFormat);
	builder._renderInfo.colorAttachmentCount = static_cast<uint32_t>(key.colorAttachmentFormats.size());
	builder._renderInfo.pColorAttachmentFormats = key.colorAttachmentFormats.data();

	// default dynamic states
	std::vector<VkDynamicState> dynamicStates = {
		VK_DYNAMIC_STATE_VIEWPORT, 
		VK_DYNAMIC_STATE_SCISSOR,
		VK_DYNAMIC_STATE_CULL_MODE
	};
	builder.set_dynamic_states(dynamicStates);

	VkPipeline pipeline = builder.build_pipeline(_core->get_device());
	_pipelineCache[key] = pipeline;

	return pipeline;
}

bool MaterialTemplate::doLoad()
{
	if (!_vertShader.is_valid() || !_fragShader.is_valid()) {
		fmt::println("MaterialTemplate: invalid shaders provided for {}", get_id());
		return false;
	}

	ReflectedShaderStage vertStage, fragStage;
	if (!ShaderReflection::reflect_stage(_vertShader->get_spirv_code(), VK_SHADER_STAGE_VERTEX_BIT, vertStage))
		return false;
	if (!ShaderReflection::reflect_stage(_fragShader->get_spirv_code(), VK_SHADER_STAGE_FRAGMENT_BIT, fragStage))
		return false;

	std::vector<ReflectedShaderStage> stages = { vertStage, fragStage };
	_reflection = ShaderReflection::merge_stages(stages);

	// compute material block size from largest member offset + size
	_materialBlockSize = 0;
	for (const auto& [name, var] : _reflection.materialVariablesByName) {
		if (name.find("materials.") == 0) {
			_materialBlockSize = std::max(_materialBlockSize, var.offset + var.size);
		}
	}
	_materialBlockSize = (_materialBlockSize + 15) & ~15; // align to 16 bytes

	if (_materialBlockSize == 0) { // should not happen
		_materialBlockSize = sizeof(MaterialConstants); 
	}

	// get layouts from reflection
	auto& cache = _resourceManager->get_pipeline_layout_cache();
	uint32_t maxSet = 0;
	for (const auto& [setIndex, _] : _reflection.sets) {
		maxSet = std::max(maxSet, setIndex);
	}

	// create descriptor set layouts
	if (!_reflection.sets.empty()) {
		_descriptorSetLayouts.resize(maxSet + 1, VK_NULL_HANDLE);

		for (const auto& [setIndex, bindings] : _reflection.sets) {
			if (setIndex == 1) {
				_descriptorSetLayouts[setIndex] = _resourceManager->get_bindless_layout();
			}
			else {
				VkDescriptorSetLayout layout = cache.get_descriptor_set_layout(bindings, false);
				_descriptorSetLayouts[setIndex] = layout;
			}
		}

		for (uint32_t i = 0; i <= maxSet; i++) {
			if (_descriptorSetLayouts[i] == VK_NULL_HANDLE) {
				_descriptorSetLayouts[i] = cache.get_descriptor_set_layout({}, false);
			}
		}
	}

	_pipelineLayout = cache.get_pipeline_layout(_descriptorSetLayouts, _reflection.pushConstantRanges);

	return true;
}

bool MaterialTemplate::doUnload()
{
	_pipelineLayout = VK_NULL_HANDLE;
	_descriptorSetLayouts.clear();

	for (auto& [key, pipeline] : _pipelineCache) {
		if (pipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(_core->get_device(), pipeline, nullptr);
		}
	}
	_pipelineCache.clear();

	return true;
}
