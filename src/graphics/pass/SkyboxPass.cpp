#include "SkyboxPass.h"
#include <graphics/reflection/DescriptorBinder.h>
#include <graphics/Renderer.h>
#include <chrono>

void SkyboxPass::init(VulkanCore* vulkanCore, ResourceManager* resourceManager, VkDescriptorSetLayout materialLayout, VkFormat colorFormat, VkFormat depthFormat)
{
	_vulkanCore = vulkanCore;

	// load shaders
	auto vert = resourceManager->load<Shader>("skybox.vert", VK_SHADER_STAGE_VERTEX_BIT, "main");
	auto frag = resourceManager->load<Shader>("skybox.frag", VK_SHADER_STAGE_FRAGMENT_BIT, "main");

	// create pipeline
	GraphicsPipelineDesc pipelineDesc{
		.shaders = {vert, frag},
		
		.colorAttachmentFormats = {colorFormat},
		.depthAttachmentFormat = depthFormat,
		.depthState = {.depthTest = true, .depthWrite = false, .compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL}, // test GREATER_OR_EQUAL to draw only on empty pixels without writing depth
		.rasterState = {.cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_CLOCKWISE, .polygonMode = VK_POLYGON_MODE_FILL},
		.blendMode = BlendMode::None
	};
	_pipeline = GraphicsPipeline(_vulkanCore->get_device(), resourceManager, pipelineDesc);
}

void SkyboxPass::cleanup()
{
	VkDevice dev = _vulkanCore->get_device();
	_pipeline.cleanup();
	_vulkanCore = nullptr;
}

void SkyboxPass::render(RenderContext& ctx, FrameData& frameData, DrawContext& drawCtx, const GPUSceneData& sceneData, const SkyboxPassParameters& params, const RenderGraph& graph)
{
	// begin clock
	auto start = std::chrono::system_clock::now();

	const EnvironmentResources& env = drawCtx.environment;

	VkDescriptorSet skyboxSet = frameData._frameDescriptors.allocate(_vulkanCore->get_device(), _pipeline.get_descriptor_set_layout(0));

	std::unordered_map<std::string, VkDescriptorBufferInfo> buffers = {
			{"sceneData", { frameData.sceneDataBuffer->get_buffer(), 0, sizeof(GPUSceneData) }}
	};
	std::unordered_map<std::string, VkDescriptorImageInfo> images = {
			{"skyMap", { env.cubemapSampler, env.environmentView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }}
	};

	DescriptorBinder::update_set(_vulkanCore->get_device(), skyboxSet, _pipeline.get_reflection().sets.at(0), buffers, images);

	ctx.bind_pipeline(_pipeline);
	ctx.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.get_layout(), 0, skyboxSet);

	// draw full scren triangle
	ctx.set_cull_mode(VK_CULL_MODE_NONE);
	ctx.draw(3, 1, 0, 0);

	auto end = std::chrono::system_clock::now();
	_stats.clear();
	_stats.name = "Skybox Pass";
	_stats.passTimeMs = std::chrono::duration<float, std::milli>(end - start).count();
}
