#include "SSAOPass.h"

#include <utils/vk_descriptors.h>
#include <graphics/Renderer.h>
#include <random>
#include <chrono>

extern AutoCVar_Float cvSSAORadius;
extern AutoCVar_Float cvSSAOBias;
extern AutoCVar_Float cvSSAOPower;

void SSAOPass::init(VulkanCore* vulkanCore, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat)
{
	_vulkanCore = vulkanCore;

	generate_kernal_and_noise();

	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); // binding 0: uniform scene data
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); // binding 1: kernel samples[64]
	builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 2: gPosition
	builder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 3: gNormal
	builder.add_binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // texNoise
	_descriptorLayout = builder.build(_vulkanCore->get_device(), VK_SHADER_STAGE_FRAGMENT_BIT);

	auto vert = Shader::load(_vulkanCore->get_device(), VK_SHADER_STAGE_VERTEX_BIT, "../../shaders/deferred_lighting.vert.spv");
	auto frag = Shader::load(_vulkanCore->get_device(), VK_SHADER_STAGE_FRAGMENT_BIT, "../../shaders/ssao.frag.spv");

	GraphicsPipelineDesc desc{
		.shaders = { vert, frag },
		.descriptorLayouts = { _descriptorLayout },
		.pushConstants = {{.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(SSAOPushConstants) }},
		.colorAttachmentFormats = { colorFormat },
		.depthAttachmentFormat = VK_FORMAT_UNDEFINED,
		.depthState = {.depthTest = false, .depthWrite = false },
		.rasterState = {.cullMode = VK_CULL_MODE_NONE, .polygonMode = VK_POLYGON_MODE_FILL }
	};
	_pipeline = GraphicsPipeline(_vulkanCore->get_device(), desc);
}

void SSAOPass::cleanup()
{
	_pipeline.cleanup();

	if (_descriptorLayout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(_vulkanCore->get_device(), _descriptorLayout, nullptr);
		_descriptorLayout = VK_NULL_HANDLE;
	}
	if (_noiseSampler != VK_NULL_HANDLE) {
		vkDestroySampler(_vulkanCore->get_device(), _noiseSampler, nullptr);
		_noiseSampler = VK_NULL_HANDLE;
	}
	if (_noiseImage.image != VK_NULL_HANDLE) {
		_vulkanCore->destroy_image(_noiseImage);
		_noiseImage = {};
	}
	if (_kernelBuffer.buffer != VK_NULL_HANDLE) {
		_vulkanCore->destroy_buffer(_kernelBuffer);
		_kernelBuffer = {};
	}

	_vulkanCore = nullptr;
}

void SSAOPass::render(RenderContext & ctx, FrameData & frameData, RenderResourceManager & resources, DrawContext & drawCtx, const GPUSceneData & sceneData)
{
	// begin clock
	auto start = std::chrono::system_clock::now();

	VkDescriptorSet ssaoSet = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _descriptorLayout);
	DescriptorWriter writer;
	writer.write_buffer(0, frameData.sceneDataBuffer.buffer, sizeof(GPUSceneData), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	writer.write_buffer(1, _kernelBuffer.buffer, sizeof(glm::vec4) * 64, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	writer.write_image(2, resources.get_view("GBufferPosition"), _vulkanCore->_defaultSamplerNearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.write_image(3, resources.get_view("GBufferNormal"), _vulkanCore->_defaultSamplerNearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.write_image(4, _noiseImage.imageView, _noiseSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.update_set(_vulkanCore->get_device(), ssaoSet);

	ctx.bind_pipeline(_pipeline);
	ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.get_layout(), 0, ssaoSet);

	// set push constants
	SSAOPushConstants push{
		.radius = static_cast<float>(cvSSAORadius.Get()),
		.bias = static_cast<float>(cvSSAOBias.Get()),
		.power = static_cast<float>(cvSSAOPower.Get()),
		.kernelSize = 64
	};
	ctx.push_constants(_pipeline.get_layout(), VK_SHADER_STAGE_FRAGMENT_BIT, push);

	// draw full screen triangle
	ctx.draw(3, 1, 0, 0);

	auto end = std::chrono::system_clock::now();
	_stats.clear();
	_stats.name = "SSAO Generation pass";
	_stats.passTimeMs = std::chrono::duration<float, std::milli>(end - start).count();
}

float lerp(float a, float b, float f)
{
	return a + f * (b - a);
}

void SSAOPass::generate_kernal_and_noise()
{
	std::uniform_real_distribution<float> randomFloats(0.0, 1.0); // generate random floats between 0.0 and 1.0
	std::default_random_engine generator;

	constexpr int KERNEL_SIZE = 64;

	// generate sample kernel
	std::vector<glm::vec4> ssaoKernel;
	ssaoKernel.reserve(KERNEL_SIZE);
	for (uint32_t i = 0; i < KERNEL_SIZE; i++) {
		glm::vec3 sample(randomFloats(generator) * 2.0 - 1.0, randomFloats(generator) * 2.0 - 1.0, randomFloats(generator));
		sample = glm::normalize(sample);
		sample *= randomFloats(generator);

		float scale = static_cast<float>(i) / static_cast<float>(KERNEL_SIZE);

		// scale samples such that theyre more aligned to center of kernel
		scale = lerp(0.1f, 1.0f, scale * scale);
		sample *= scale;
		
		ssaoKernel.emplace_back(sample, 1.0);
	}

	_kernelBuffer = _vulkanCore->create_buffer(sizeof(glm::vec4) * KERNEL_SIZE, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	memcpy(_kernelBuffer.info.pMappedData, ssaoKernel.data(), sizeof(glm::vec4) * KERNEL_SIZE);

	constexpr int NOISE_DIM = 16;
	// generate noise texture
	std::vector<glm::vec4> ssaoNoise;
	ssaoNoise.reserve(NOISE_DIM * NOISE_DIM);
	for (uint32_t i = 0; i < NOISE_DIM * NOISE_DIM; i++) {
		glm::vec3 noise(randomFloats(generator) * 2.0 - 1.0, randomFloats(generator) * 2.0 - 1.0, 0.0f); // rotate around z axis (in tangent space)
		ssaoNoise.emplace_back(noise, 1.0);
	}

	AllocatedBuffer staging = _vulkanCore->create_buffer(sizeof(glm::vec4) * NOISE_DIM * NOISE_DIM, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	memcpy(staging.info.pMappedData, ssaoNoise.data(), sizeof(glm::vec4) * NOISE_DIM * NOISE_DIM);

	ImageDescription desc{
		.extent = { 4, 4, 1 },
		.format = VK_FORMAT_R32G32B32A32_SFLOAT,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.mipLevels = 1,
		.arrayLayers = 1,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
	};
	_noiseImage = _vulkanCore->create_image(desc);

	// copy noise data staging buffer to noise image
	_vulkanCore->immediate_submit([&](VkCommandBuffer cmd) {
		RenderSync::transition_image(cmd, _noiseImage.image, ResourceState::Undefined, ResourceState::TransferDst);
		VkBufferImageCopy copyRegion{
			.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
			.imageExtent = { 4, 4, 1}
		};
		vkCmdCopyBufferToImage(cmd, staging.buffer, _noiseImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
		RenderSync::transition_image(cmd, _noiseImage.image, ResourceState::TransferDst, ResourceState::ShaderReadOnly);
		});

	// destroy staging buffer
	_vulkanCore->destroy_buffer(staging);

	// repeat sampler for tiling across screen
	VkSamplerCreateInfo samplerInfo{ .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samplerInfo.magFilter = VK_FILTER_NEAREST;
	samplerInfo.minFilter = VK_FILTER_NEAREST;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	VK_CHECK(vkCreateSampler(_vulkanCore->get_device(), &samplerInfo, nullptr, &_noiseSampler));
}
