#include "SSAOPass.h"

#include <graphics/reflection/DescriptorBinder.h>
#include <graphics/reflection/PushConstantDispatcher.h>
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

	auto vert = resourceManager->load<Shader>("deferred_lighting.vert", VK_SHADER_STAGE_VERTEX_BIT, "main");
	auto frag = resourceManager->load<Shader>("ssao.frag", VK_SHADER_STAGE_FRAGMENT_BIT, "main");

	GraphicsPipelineDesc desc{
		.shaders = { vert, frag },
		.colorAttachmentFormats = { colorFormat },
		.depthAttachmentFormat = VK_FORMAT_UNDEFINED,
		.depthState = {.depthTest = false, .depthWrite = false },
		.rasterState = {.cullMode = VK_CULL_MODE_NONE, .polygonMode = VK_POLYGON_MODE_FILL }
	};
	_pipeline = GraphicsPipeline(_vulkanCore->get_device(), resourceManager, desc);
}

void SSAOPass::cleanup()
{
	_pipeline.cleanup();
	_vulkanCore = nullptr;
}

void SSAOPass::render(RenderContext& ctx, FrameData& frameData, DrawContext& drawCtx, const GPUSceneData& data, const SSAOPassParameters& params, const RenderGraph& graph)
{
	VkDescriptorSet ssaoSet = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _pipeline.get_descriptor_set_layout(0));

	std::unordered_map<std::string, VkDescriptorBufferInfo> buffers = {
			{"sceneData", { frameData.sceneDataBuffer->get_buffer(), 0, sizeof(GPUSceneData) }},
			{"kernel", { _kernelBuffer->get_buffer(), 0, sizeof(glm::vec4) * 64 }}
	};

	std::unordered_map<std::string, VkDescriptorImageInfo> images = {
			{"gPosition", { DefaultResources::get().defaultSamplerLinear->get_sampler(), graph.get_image_view(params.gPosition), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }},
			{"gNormal", { DefaultResources::get().defaultSamplerNearest->get_sampler(), graph.get_image_view(params.gNormal), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }},
			{"texNoise", { _noiseSampler->get_sampler(), _noiseImage->get_image_view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }}
	};

	DescriptorBinder::update_set(_vulkanCore->get_device(), ssaoSet, _pipeline.get_reflection().sets.at(0), buffers, images);

	ctx.bind_pipeline(_pipeline);
	ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.get_layout(), 0, ssaoSet);

	PushConstantDispatcher pc(_pipeline.get_reflection());
	pc.set("radius", static_cast<float>(cvSSAORadius.Get()));
	pc.set("bias", static_cast<float>(cvSSAOBias.Get()));
	pc.set("power", static_cast<float>(cvSSAOPower.Get()));
	pc.set("kernelSize", 64);
	pc.flush(ctx.get_cmd(), _pipeline.get_layout());

	//  draw full screen triangle
	ctx.set_cull_mode(VK_CULL_MODE_NONE);
	ctx.draw(3, 1, 0, 0);
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
