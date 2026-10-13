#pragma once

#include <graphics/utils/vk_types.h>
#include <graphics/Pipeline.h>
#include <string>
#include <filesystem>
#include <resources/Texture.h>
#include <resources/Sampler.h>

class VulkanCore;

// manages all IBL resources and pipelines
class IBLSystem
{
public:
	void init(VulkanCore* vulkanCore, ResourceManager* resourceManager);
	void cleanup();

	// load an hdr image
	bool load_hdr(const std::filesystem::path& path);

	// fallback if no hdr is loaded
	void load_procedural_default();

	EnvironmentResources get_environment_resources() const {
		return EnvironmentResources{
			.environmentView = _envCubemap->get_image_view(),
			.irradianceView = _irradianceCubemap->get_image_view(),
			.prefilterView = _prefilterCubemap->get_image_view(),
			.brdfLutView = _brdfLut->get_image_view(),
			.cubemapSampler = _cubemapSampler->get_sampler(),
			.lutSampler = _lutSampler->get_sampler()
		};
	}

private:
	void init_samplers();
	void init_pipelines(ResourceManager* resourceManager);
	void generate_brdf_lut();
	void destroy_current_environment();

	void generate_cubemap_mips(VkCommandBuffer cmd, VkImage image, uint32_t size, uint32_t mipLevels, VkImageLayout layout, VkFormat format);

	VulkanCore* _vulkanCore;
	ResourceManager* _resourceManager;

	ResourceHandle<Texture> _envCubemap;
	ResourceHandle<Texture> _irradianceCubemap;
	ResourceHandle<Texture> _prefilterCubemap;
	ResourceHandle<Texture> _brdfLut;

	ResourceHandle<Sampler> _cubemapSampler;
	ResourceHandle<Sampler> _lutSampler;
	ResourceHandle<Sampler> _hdrSampler;

	// Compute pipelines
	ComputePipeline _equirectToCubemapPipeline;
	ComputePipeline _irradiancePipeline;
	ComputePipeline _prefilterPipeline;
	ComputePipeline _brdfLutPipeline;

	bool _isHdrLoaded{ false };
	std::string _hdrPath{ "Procedural sky" };
};