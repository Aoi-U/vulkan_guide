#include "SSAOPass.h"

#include <utils/vk_descriptors.h>
#include <graphics/Renderer.h>
#include <graphics/utils/vk_utils.h>
#include <resources/DefaultResources.h>
#include <resources/ResourceManager.h>
#include <random>
#include <chrono>

extern AutoCVar_Float cvSSAORadius;
extern AutoCVar_Float cvSSAOBias;
extern AutoCVar_Float cvSSAOPower;

void SSAOPass::init(VulkanCore* vulkanCore, ResourceManager* resourceManager, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat)
{
	_vulkanCore = vulkanCore;
	_resourceManager = resourceManager;

	generate_kernal_and_noise();

	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); // binding 0: uniform scene data
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); // binding 1: kernel samples[64]
	builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 2: gPosition
	builder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 3: gNormal
	builder.add_binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // texNoise
	_descriptorLayout = builder.build(_vulkanCore->get_device(), VK_SHADER_STAGE_FRAGMENT_BIT);

	auto vert = resourceManager->load<Shader>("deferred_lighting.vert", VK_SHADER_STAGE_VERTEX_BIT, "main");
	auto frag = resourceManager->load<Shader>("ssao.frag", VK_SHADER_STAGE_FRAGMENT_BIT, "main");

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
	//if (_noiseSampler != VK_NULL_HANDLE) {
	//	vkDestroySampler(_vulkanCore->get_device(), _noiseSampler, nullptr);
	//	_noiseSampler = VK_NULL_HANDLE;
	//}
	
	//_noiseImage.unload();
	//_kernelBuffer.unload();

	_vulkanCore = nullptr;
}

void SSAOPass::render(RenderContext& ctx, FrameData& frameData, DrawContext& drawCtx, const GPUSceneData& data, const SSAOPassParameters& params, const RenderGraph& graph)
{
	// begin clock
	auto start = std::chrono::system_clock::now();

	VkDescriptorSet ssaoSet = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _descriptorLayout);
	DescriptorWriter writer;
	writer.write_buffer(0, frameData.sceneDataBuffer->get_buffer(), sizeof(GPUSceneData), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	writer.write_buffer(1, _kernelBuffer->get_buffer(), sizeof(glm::vec4) * 64, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	writer.write_image(2, graph.get_image_view(params.gPosition), DefaultResources::get().defaultSamplerLinear->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.write_image(3, graph.get_image_view(params.gNormal), DefaultResources::get().defaultSamplerNearest->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.write_image(4, _noiseImage->get_image_view(), _noiseSampler->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
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
	ctx.set_push_constants(_pipeline.get_layout(), VK_SHADER_STAGE_FRAGMENT_BIT, push);

	//  draw full screen triangle
	ctx.draw(3, 1, 0, 0);

	auto end = std::chrono::system_clock::now();
	_stats.clear();
	_stats.name = "SSAO Generation pass";
	_stats.passTimeMs = std::chrono::duration<float, std::milli>(end - start).count();
}

namespace {
	float lerp(float a, float b, float f)
	{
		return a + f * (b - a);
	}
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

	_kernelBuffer = _resourceManager->load<Buffer>("ssao_kernel", std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(ssaoKernel.data()), sizeof(glm::vec4) * KERNEL_SIZE), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	constexpr int NOISE_DIM = 16;
	// generate noise texture
	std::vector<glm::vec4> ssaoNoise;
	ssaoNoise.reserve(NOISE_DIM * NOISE_DIM);
	for (uint32_t i = 0; i < NOISE_DIM * NOISE_DIM; i++) {
		glm::vec3 noise(randomFloats(generator) * 2.0 - 1.0, randomFloats(generator) * 2.0 - 1.0, 0.0f); // rotate around z axis (in tangent space)
		ssaoNoise.emplace_back(noise, 1.0);
	}

	ResourceHandle<Buffer> staging = _resourceManager->load<Buffer>("ssao_noise_staging", std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(ssaoNoise.data()), sizeof(glm::vec4) * NOISE_DIM * NOISE_DIM), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	staging->copy_data(ssaoNoise.data(), sizeof(glm::vec4) * NOISE_DIM * NOISE_DIM);

	TextureDescription desc{
		.extent = { 4, 4, 1 },
		.format = VK_FORMAT_R32G32B32A32_SFLOAT,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.mipLevels = 1,
		.arrayLayers = 1,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
	};

	_noiseImage = _resourceManager->load<Texture>("ssao_noise", desc);

	// copy noise data staging buffer to noise image
	_vulkanCore->immediate_submit([&](VkCommandBuffer cmd) {
		vkutil::image_mem_barrier2(cmd, _noiseImage->get_image(), VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		VkBufferImageCopy copyRegion{
			.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
			.imageExtent = { 4, 4, 1}
		};
		vkCmdCopyBufferToImage(cmd, staging->get_buffer(), _noiseImage->get_image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
		vkutil::image_mem_barrier2(cmd, _noiseImage->get_image(), VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		});

	// destroy staging buffer
	//staging.destroy_immediate();

	// repeat sampler for tiling across screen
	VkSamplerCreateInfo samplerInfo{ .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samplerInfo.magFilter = VK_FILTER_NEAREST;
	samplerInfo.minFilter = VK_FILTER_NEAREST;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	//VK_CHECK(vkCreateSampler(_vulkanCore->get_device(), &samplerInfo, nullptr, &_noiseSampler));
	_noiseSampler = _resourceManager->load<Sampler>("ssao_noise_sampler", samplerInfo);
}
