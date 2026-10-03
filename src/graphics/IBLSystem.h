#pragma once

#include <core/vk_types.h>
#include <graphics/Pipeline.h>
#include <string>
#include <filesystem>
#include <graphics/utils/RenderSync.h>

class VulkanCore;

// manages all IBL resources and pipelines
class IBLSystem
{
public:
	void init(VulkanCore* vulkanCore);
	void cleanup();

	// load an hdr image
	bool load_hdr(const std::filesystem::path& path);

	// fallback if no hdr is loaded
	void load_procedural_default();

	EnvironmentResources get_environment_resources() const {
		return EnvironmentResources{
			.environmentView = _envCubemap.imageView,
			.irradianceView = _irradianceCubemap.imageView,
			.prefilterView = _prefilterCubemap.imageView,
			.brdfLutView = _brdfLut.imageView,
			.cubemapSampler = _cubemapSampler,
			.lutSampler = _lutSampler
		};
	}

private:
	void init_samplers();
	void init_pipelines();
	void generate_brdf_lut();
	void destroy_current_environment();

	void generate_cubemap_mips(VkCommandBuffer cmd, VkImage image, uint32_t size, uint32_t mipLevels, ResourceState baseState);

	VulkanCore* _vulkanCore;

	AllocatedImage _envCubemap;
	AllocatedImage _irradianceCubemap;
	AllocatedImage _prefilterCubemap;
	AllocatedImage _brdfLut;

	VkSampler _cubemapSampler;
	VkSampler _lutSampler;
	VkSampler _hdrSampler;

	// Compute pipelines
	ComputePipeline _equirectToCubemapPipeline;
	ComputePipeline _irradiancePipeline;
	ComputePipeline _prefilterPipeline;
	ComputePipeline _brdfLutPipeline;

	VkDescriptorSetLayout _computeLayout;
	VkDescriptorSetLayout _lutComputeLayout;

	bool _isHdrLoaded{ false };
	std::string _hdrPath{ "Procedural sky" };
};