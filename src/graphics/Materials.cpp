#include "Materials.h"
#include <graphics/VulkanCore.h>
#include <utils/vk_initializers.h>
#include <utils/vk_pipelines.h>
#include <utils/vk_utils.h>

void GLTFMetallic_Roughness::build_pipelines(VkDevice device, VkDescriptorSetLayout sceneDataLayout, VkFormat colorFormat, VkFormat depthFormat)
{
	VkShaderModule meshFragShader;
	if (!vkutil::load_shader_module("../../shaders/mesh.frag.spv", device, &meshFragShader)) {
		fmt::println("Error when building the mesh fragment shader module");

	}

	VkShaderModule meshVertexShader;
	if (!vkutil::load_shader_module("../../shaders/mesh.vert.spv", device, &meshVertexShader)) {
		fmt::println("Error when building the mesh vertex shader module");
	}

	// setup push constant range for transform matrix
	VkPushConstantRange matrixRange{};
	matrixRange.offset = 0;
	matrixRange.size = sizeof(GPUDrawPushConstants);
	matrixRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

	// build descriptorsetlayout for material
	DescriptorLayoutBuilder layoutBuilder;
	layoutBuilder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); // binding 0: uniform buffer for MaterialConstants
	layoutBuilder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 1: combined image sampler for base color texture
	layoutBuilder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 2: combined image sampler for metallic-roughness (pbr) texture
	materialLayout = layoutBuilder.build(device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

	// create pipeline layout with the scene data layout and material layout
	VkDescriptorSetLayout layouts[] = { sceneDataLayout, materialLayout };
	VkPipelineLayoutCreateInfo meshLayoutInfo = vkinit::pipeline_layout_create_info();
	meshLayoutInfo.setLayoutCount = 2;
	meshLayoutInfo.pSetLayouts = layouts;
	meshLayoutInfo.pPushConstantRanges = &matrixRange;
	meshLayoutInfo.pushConstantRangeCount = 1;

	VkPipelineLayout meshPipelineLayout;
	VK_CHECK(vkCreatePipelineLayout(device, &meshLayoutInfo, nullptr, &meshPipelineLayout));

	opaquePipeline.layout = meshPipelineLayout;
	transparentPipeline.layout = meshPipelineLayout;

	// build the stage create info for both vertex and fragment stages
	// lets the pipeline know the shader modules per stage
	PipelineBuilder pipelineBuilder;
	pipelineBuilder.set_shaders(meshVertexShader, meshFragShader);
	pipelineBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	pipelineBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	pipelineBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	pipelineBuilder.set_multisampling_none();
	pipelineBuilder.disable_blending();
	pipelineBuilder.enable_depthtest(true, VK_COMPARE_OP_GREATER_OR_EQUAL);
	pipelineBuilder.set_color_attachment_format(colorFormat);
	pipelineBuilder.set_depth_format(depthFormat);
	pipelineBuilder._pipelineLayout = meshPipelineLayout;

	// build the opaque pipeline
	opaquePipeline.pipeline = pipelineBuilder.build_pipeline(device);

	// build the transparent pipeline
	// same as opaque but with blending enabled
	pipelineBuilder.enable_blending_additive();
	pipelineBuilder.enable_depthtest(false, VK_COMPARE_OP_GREATER_OR_EQUAL);
	transparentPipeline.pipeline = pipelineBuilder.build_pipeline(device);

	// clean up shader modules
	vkDestroyShaderModule(device, meshFragShader, nullptr);
	vkDestroyShaderModule(device, meshVertexShader, nullptr);
}

void GLTFMetallic_Roughness::clear_resources(VkDevice device)
{
	vkDestroyDescriptorSetLayout(device, materialLayout, nullptr);
	vkDestroyPipelineLayout(device, transparentPipeline.layout, nullptr);

	vkDestroyPipeline(device, transparentPipeline.pipeline, nullptr);
	vkDestroyPipeline(device, opaquePipeline.pipeline, nullptr);
}

MaterialInstance GLTFMetallic_Roughness::write_material(VkDevice device, MaterialPass pass, const MaterialResources& resources, DescriptorAllocatorGrowable& descriptorAllocator)
{
	// write the material data to the descriptor set
	MaterialInstance matData;
	matData.passType = pass;
	matData.pipeline = (pass == MaterialPass::Transparent) ? &transparentPipeline : &opaquePipeline;
	matData.materialSet = descriptorAllocator.allocate(device, materialLayout);

	writer.clear();
	writer.write_buffer(0, resources.dataBuffer, sizeof(MaterialConstants), resources.dataBufferOffset, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	writer.write_image(1, resources.colorImage.imageView, resources.colorSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.write_image(2, resources.metalRoughImage.imageView, resources.metalRoughSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.update_set(device, matData.materialSet);

	return matData;
}
