#include "ForwardPass.h"

#include <utils/vk_descriptors.h>
#include <graphics/Renderer.h>
#include <chrono>

void ForwardPass::init(VulkanCore* vulkanCore, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat)
{
	_vulkanCore = vulkanCore;

	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); // binding 0: uniform buffer for scene data
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // binding 1: storage buffer for object data
	builder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); // binding 2: storage buffer for instance data
	builder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 3: combined image sampler for shadow map
	builder.add_binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 4: combined image sampler for irradiance map
	builder.add_binding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 5: combined image sampler for prefiltered environment map
	builder.add_binding(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 6: combined image sampler for brdf lut
	_sceneDataDescriptorLayout = builder.build(_vulkanCore->get_device(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

	// load shaders
	auto vert = Shader::load(_vulkanCore->get_device(), VK_SHADER_STAGE_VERTEX_BIT, "../../shaders/mesh.vert.spv");
	auto frag = Shader::load(_vulkanCore->get_device(), VK_SHADER_STAGE_FRAGMENT_BIT, "../../shaders/mesh.frag.spv");

	// create opaque pipeline
	GraphicsPipelineDesc opaqueDesc{
		.shaders = {vert, frag},
		.descriptorLayouts = {_sceneDataDescriptorLayout, materialLayout},
		.colorAttachmentFormats = {colorFormat},
		.depthAttachmentFormat = depthFormat,
		.depthState = {.depthTest = true, .depthWrite = true, .compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL},
		.rasterState = {.cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_CLOCKWISE, .polygonMode = VK_POLYGON_MODE_FILL},
		.blendMode = BlendMode::None
	};
	_opaquePipeline = GraphicsPipeline(_vulkanCore->get_device(), opaqueDesc);

	// create transparent pipeline
	GraphicsPipelineDesc transparentDesc = opaqueDesc;
	transparentDesc.depthState.depthWrite = false;
	transparentDesc.blendMode = BlendMode::Additive;
	_transparentPipeline = GraphicsPipeline(_vulkanCore->get_device(), transparentDesc);
}

void ForwardPass::cleanup()
{
	_opaquePipeline.cleanup();
	_transparentPipeline.cleanup();
	if (_sceneDataDescriptorLayout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(_vulkanCore->get_device(), _sceneDataDescriptorLayout, nullptr);
		_sceneDataDescriptorLayout = VK_NULL_HANDLE;
	}
	_vulkanCore = nullptr;
}

void ForwardPass::render(RenderContext& ctx, FrameData & frameData, RenderResourceManager& resources, DrawContext & drawCtx, const GPUSceneData & sceneData)
{	
	if (drawCtx.indirectDrawCount == 0 || frameData._gpuIndirectBuffer.buffer == VK_NULL_HANDLE || drawCtx.opaqueBatches.empty()) {
		return;
	}

	// begin clock
	auto start = std::chrono::system_clock::now();

	const EnvironmentResources& env = drawCtx.environment;

	// create a descriptor set that binds the scene data buffer and update it
	VkDescriptorSet globalDescriptor = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _sceneDataDescriptorLayout);

	DescriptorWriter writer;
	writer.write_buffer(0, frameData.sceneDataBuffer.buffer, sizeof(GPUSceneData), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	writer.write_buffer(1, frameData._gpuObjectBuffer.buffer, sizeof(GPUObjectData) * drawCtx.indirectDrawCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.write_buffer(2, frameData._gpuInstanceBuffer.buffer, sizeof(uint32_t) * drawCtx.indirectDrawCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.write_image(3, resources.get_view("ShadowMap"), _vulkanCore->_shadowSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.write_image(4, env.irradianceView, env.cubemapSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.write_image(5, env.prefilterView, env.cubemapSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.write_image(6, env.brdfLutView, env.lutSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.update_set(_vulkanCore->get_device(), globalDescriptor);

	// bind pipeline and descriptor set
	ctx.bind_pipeline(_opaquePipeline);
	ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _opaquePipeline.get_layout(), 0, globalDescriptor);

	MaterialInstance* lastMaterial = nullptr;
	VkBuffer lastIndexBuffer = VK_NULL_HANDLE;

	for (const IndirectBatch& batch : drawCtx.opaqueBatches) {
		if (!batch.material) continue;	

		// rebind material descriptor set if needed
		if (batch.material != lastMaterial) {
			lastMaterial = batch.material;
			ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _opaquePipeline.get_layout(), 1, lastMaterial->materialSet);
		}

		// rebind index buffer if needed
		if (batch.indexBuffer != lastIndexBuffer) {
			lastIndexBuffer = batch.indexBuffer;
			ctx.bind_index_buffer(batch.indexBuffer);
		}

		// draw batch
		VkDeviceSize indirectOffset = batch.first * sizeof(VkDrawIndexedIndirectCommand);
		VkDeviceSize countOffset = batch.batchIndex * sizeof(uint32_t);

		ctx.draw_indexed_indirect_count(frameData._gpuIndirectBuffer.buffer, indirectOffset, frameData._gpuCountBuffer.buffer, countOffset, batch.count, sizeof(VkDrawIndexedIndirectCommand));
	}
	
	auto end = std::chrono::system_clock::now(); 
	_stats.clear(); 
	_stats.name = "Forward Pass"; 
	_stats.passTimeMs = std::chrono::duration<float, std::milli>(end - start).count(); 
}
