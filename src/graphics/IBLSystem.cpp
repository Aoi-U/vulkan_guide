#include "graphics/IBLSystem.h"

#include <graphics/VulkanCore.h>
#include <utils/vk_initializers.h>
#include <utils/vk_descriptors.h>
#include <graphics/utils/vk_utils.h>
#include <resources/Buffer.h>
#include <stb_image.h>
#include <fmt/core.h>
#include <cmath>

struct PrefilterPushConstants
{
	float roughness;
	float resolution;
};

void IBLSystem::init(VulkanCore* vulkanCore, ResourceManager* resourceManager)
{
	_resourceManager = resourceManager;
	_vulkanCore = vulkanCore;
	init_samplers();
	init_pipelines(resourceManager);
	generate_brdf_lut();
	load_procedural_default();
}

void IBLSystem::cleanup()
{
	VkDevice device = _vulkanCore->get_device();
	destroy_current_environment();

	_cubemapSampler = {};
	_lutSampler = {};
	_hdrSampler = {};

	_equirectToCubemapPipeline.cleanup();
	_irradiancePipeline.cleanup();
	_prefilterPipeline.cleanup();
	_brdfLutPipeline.cleanup();
}

bool IBLSystem::load_hdr(const std::filesystem::path& path)
{
	if (!std::filesystem::exists(path)) {
		fmt::println("IBLSystem: File not found: {}", path.string());
		return false;
	}

	int width, height, channels;
	float* data = stbi_loadf(path.string().c_str(), &width, &height, &channels, 4);
	if (!data) {
		fmt::println("IBLSystem: Failed to load HDR image: {}", path.string());
		return false;
	}

	// destroy existing environment
	destroy_current_environment();

	// upload hdr image to staging buffer
	size_t dataBytes = width * height * 4 * sizeof(float);
	//Buffer staging("hdr_staging", dataBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	//staging.load();
	//staging.copy_data(data, dataBytes);
	ResourceHandle<Buffer> staging = _resourceManager->load<Buffer>("hdr_staging", dataBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	staging->copy_data(data, dataBytes);

	// free image data after uploading
	stbi_image_free(data);

	// create the hdr image
	TextureDescription hdrDesc{ .extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1}, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .aspectMask = static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT) };
	//Texture hdrImage("hdr_img", hdrDesc);
	//hdrImage.load();
	ResourceHandle<Texture> hdrImage = _resourceManager->load<Texture>("hdr_img", hdrDesc);

	// copy image from staging buffer to hdr image
	_vulkanCore->immediate_submit([&](VkCommandBuffer cmd) {
		vkutil::image_mem_barrier2(cmd, hdrImage->get_image(), hdrImage->get_format(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		VkBufferImageCopy copyRegion{ .imageSubresource = {
			.aspectMask = static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT),
			.mipLevel = 0,
			.baseArrayLayer = 0,
			.layerCount = 1},
			.imageExtent = hdrImage->get_extent()
		};

		vkCmdCopyBufferToImage(cmd, staging->get_buffer(), hdrImage->get_image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
		vkutil::image_mem_barrier2(cmd, hdrImage->get_image(), hdrImage->get_format(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		});

	// destroy staging buffer
	//staging.destroy_immediate();
	staging->unload();

	// create environment cubemap
	constexpr uint32_t ENV_CUBEMAP_SIZE = 2048;
	uint32_t envMips = static_cast<uint32_t>(std::floor(std::log2(ENV_CUBEMAP_SIZE))) + 1;

	TextureDescription envDesc{
		.extent = {ENV_CUBEMAP_SIZE, ENV_CUBEMAP_SIZE, 1},
		.format = VK_FORMAT_R16G16B16A16_SFLOAT,
		.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.mipLevels = envMips,
		.arrayLayers = 6,
		.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
		.viewType = VK_IMAGE_VIEW_TYPE_CUBE,
		.aspectMask = static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT)
	};
	//_envCubemap = Texture("envCubemap", envDesc);
	//_envCubemap.load();
	_envCubemap = _resourceManager->load<Texture>("envCubemap", envDesc);

	// temporary image view for compute output
	VkImageView mip0ArrayView;
	VkImageViewCreateInfo viewInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = _envCubemap->get_image(),
		.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
		.format = _envCubemap->get_format(),
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6}
	};
	VK_CHECK(vkCreateImageView(_vulkanCore->get_device(), &viewInfo, nullptr, &mip0ArrayView));

	// allocate descriptor set for equirectangular to cubemap compute shader
	DescriptorAllocatorGrowable allocator;
	std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> poolSizes = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 10},
		{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 10}
	};
	allocator.init(_vulkanCore->get_device(), 1, poolSizes);

	// run equirectangular to cubemap compute shader
	VkDescriptorSet eqSet = allocator.allocate(_vulkanCore->get_device(), _equirectToCubemapPipeline.get_descriptor_set_layout(0));
	DescriptorWriter eqWriter;
	eqWriter.write_image(0, mip0ArrayView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	eqWriter.write_image(1, hdrImage->get_image_view(), _hdrSampler->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	eqWriter.update_set(_vulkanCore->get_device(), eqSet);

	// dispatch compute shader to convert equirectangular hdr to cubemap
	_vulkanCore->immediate_submit([&](VkCommandBuffer cmd) {
		vkutil::image_mem_barrier2(cmd, _envCubemap->get_image(), _envCubemap->get_format(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 6, 1, 0);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _equirectToCubemapPipeline.get_pipeline());
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _equirectToCubemapPipeline.get_layout(), 0, 1, &eqSet, 0, nullptr);
		vkCmdDispatch(cmd, ENV_CUBEMAP_SIZE / 16, ENV_CUBEMAP_SIZE / 16, 6);

		// generate full mip chain for environment cubemap
		generate_cubemap_mips(cmd, _envCubemap->get_image(), ENV_CUBEMAP_SIZE, envMips, VK_IMAGE_LAYOUT_GENERAL, _envCubemap->get_format());
		});

	// cleanup temporary image view and hdr image
	vkDestroyImageView(_vulkanCore->get_device(), mip0ArrayView, nullptr);

	// create irradiance cubemap
	constexpr uint32_t IRR_SIZE = 64;

	TextureDescription irrDesc{
		.extent = { IRR_SIZE, IRR_SIZE, 1 },
		.format = VK_FORMAT_R16G16B16A16_SFLOAT,
		.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.mipLevels = 1,
		.arrayLayers = 6,
		.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
		.viewType = VK_IMAGE_VIEW_TYPE_CUBE,
		.aspectMask = static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT)
	};
	//_irradianceCubemap = Texture("irradianceCubemap", irrDesc);
	//_irradianceCubemap.load();
	_irradianceCubemap = _resourceManager->load<Texture>("irradianceCubemap", irrDesc);

	// create temporary image view for compute shader
	VkImageView irrArrayView;
	viewInfo.image = _irradianceCubemap->get_image();
	viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
	VK_CHECK(vkCreateImageView(_vulkanCore->get_device(), &viewInfo, nullptr, &irrArrayView));

	// run irradiance compute shader
	VkDescriptorSet irrSet = allocator.allocate(_vulkanCore->get_device(), _irradiancePipeline.get_descriptor_set_layout(0));
	DescriptorWriter irrWriter;
	irrWriter.write_image(0, irrArrayView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	irrWriter.write_image(1, _envCubemap->get_image_view(), _cubemapSampler->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	irrWriter.update_set(_vulkanCore->get_device(), irrSet);

	// dispatch compute shader to generate irradiance cubemap
	_vulkanCore->immediate_submit([&](VkCommandBuffer cmd) {
		vkutil::image_mem_barrier2(cmd, _irradianceCubemap->get_image(), _irradianceCubemap->get_format(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 6, 1, 0);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _irradiancePipeline.get_pipeline());
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _irradiancePipeline.get_layout(), 0, 1, &irrSet, 0, nullptr);
		vkCmdDispatch(cmd, IRR_SIZE / 16, IRR_SIZE / 16, 6);
		vkutil::image_mem_barrier2(cmd, _irradianceCubemap->get_image(), _irradianceCubemap->get_format(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 6, 1, 0);
		});
	// cleanup temporary image view
	vkDestroyImageView(_vulkanCore->get_device(), irrArrayView, nullptr);

	// specular prefilter cubemap
	constexpr uint32_t PREFILTER_SIZE = 128;
	constexpr uint32_t PREFILTER_MIPS = 5;

	TextureDescription prefDesc{
		.extent = { PREFILTER_SIZE, PREFILTER_SIZE, 1 },
		.format = VK_FORMAT_R16G16B16A16_SFLOAT,
		.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.mipLevels = PREFILTER_MIPS,
		.arrayLayers = 6,
		.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
		.viewType = VK_IMAGE_VIEW_TYPE_CUBE,
		.aspectMask = static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT)
	};
	//_prefilterCubemap = Texture("prefilterCubemap", prefDesc);
	//_prefilterCubemap.load();
	_prefilterCubemap = _resourceManager->load<Texture>("prefilterCubemap", prefDesc);

	// create temporary image views for each mip level of the prefilter cubemap
	std::vector<VkImageView> prefilterMipViews(PREFILTER_MIPS);
	for (uint32_t mip = 0; mip < PREFILTER_MIPS; ++mip) {
		VkImageViewCreateInfo mipViewInfo{
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = _prefilterCubemap->get_image(),
			.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
			.format = VK_FORMAT_R16G16B16A16_SFLOAT,
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 6 }
		};
		VK_CHECK(vkCreateImageView(_vulkanCore->get_device(), &mipViewInfo, nullptr, &prefilterMipViews[mip]));
	}

	// dispatch compute shader for each mip level
	_vulkanCore->immediate_submit([&](VkCommandBuffer cmd) {
		vkutil::image_mem_barrier2(cmd, _prefilterCubemap->get_image(), _prefilterCubemap->get_format(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 6, PREFILTER_MIPS, 0);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _prefilterPipeline.get_pipeline());

		for (uint32_t mip = 0; mip < PREFILTER_MIPS; ++mip) {
			uint32_t mipDim = PREFILTER_SIZE >> mip;

			// allocate descriptor set for this mip level
			VkDescriptorSet prefSet = allocator.allocate(_vulkanCore->get_device(), _prefilterPipeline.get_descriptor_set_layout(0));
			DescriptorWriter prefWriter;
			prefWriter.write_image(0, prefilterMipViews[mip], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
			prefWriter.write_image(1, _envCubemap->get_image_view(), _cubemapSampler->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			prefWriter.update_set(_vulkanCore->get_device(), prefSet);

			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _prefilterPipeline.get_layout(), 0, 1, &prefSet, 0, nullptr);

			// push constants for roughness and resolution
			PrefilterPushConstants push{
				.roughness = static_cast<float>(mip) / static_cast<float>(PREFILTER_MIPS - 1),
				.resolution = static_cast<float>(ENV_CUBEMAP_SIZE)
			};
			vkCmdPushConstants(cmd, _prefilterPipeline.get_layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

			vkCmdDispatch(cmd, std::max(1u, mipDim / 16), std::max(1u, mipDim / 16), 6);
		}

		// transition prefilter cubemap 
		vkutil::image_mem_barrier2(cmd, _prefilterCubemap->get_image(), _prefilterCubemap->get_format(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 6, PREFILTER_MIPS, 0);
		});

	// cleanup descriptor pools and prefilter mip views
	allocator.destroy_pools(_vulkanCore->get_device());
	for (VkImageView view : prefilterMipViews) {
		vkDestroyImageView(_vulkanCore->get_device(), view, nullptr);
	}

	_isHdrLoaded = true;
	_hdrPath = path.filename().string();
	fmt::println("IBLSystem: Loaded HDR image: {}", path.string());
	return true;
}

void IBLSystem::load_procedural_default()
{
	destroy_current_environment();

	// procedurally generated gradient cubemap 
	constexpr uint32_t FACE_SIZE = 256;
	const size_t faceBytes = FACE_SIZE * FACE_SIZE * 4 * sizeof(float);
	//Buffer staging("procedural_staging", faceBytes * 6, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	//staging.load();
	//float* mappedData = static_cast<float*>(staging.get_mapped_data());
	ResourceHandle<Buffer> staging = _resourceManager->load<Buffer>("procedural_staging", faceBytes * 6, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	float* mappedData = static_cast<float*>(staging->get_mapped_data());

	// set colors for sky and 
	const glm::vec3 zenith(0.18f, 0.38f, 0.85f);
	const glm::vec3 horizon(0.72f, 0.82f, 0.95f);
	const glm::vec3 ground(0.22f, 0.22f, 0.25f);
	const glm::vec3 sunDir = glm::normalize(glm::vec3(0.4f, 1.0f, 0.3f));

	for (uint32_t face = 0; face < 6; ++face) {
		for (uint32_t y = 0; y < FACE_SIZE; ++y) {
			for (uint32_t x = 0; x < FACE_SIZE; ++x) {
				float u = (2.0f * (x + 0.5f) / FACE_SIZE) - 1.0f;
				float v = (2.0f * (y + 0.5f) / FACE_SIZE) - 1.0f;
				glm::vec3 dir(0.0f);
				switch (face) {
				case 0: dir = glm::vec3(1.0f, -v, -u); break; // +x
				case 1: dir = glm::vec3(-1.0f, -v, u); break; // -x
				case 2: dir = glm::vec3(u, 1.0f, v); break; // +y
				case 3: dir = glm::vec3(u, -1.0f, -v); break; // -y
				case 4: dir = glm::vec3(u, -v, 1.0f); break; // +z
				case 5: dir = glm::vec3(-u, -v, -1.0f); break; // -z
				}
				dir = glm::normalize(dir);
				glm::vec3 col = (dir.y > 0.0f) ? glm::mix(horizon, zenith, dir.y) : glm::mix(horizon, ground, -dir.y);
				float sunDot = glm::max(glm::dot(dir, sunDir), 0.0f);
				col += glm::vec3(1.0f, 0.95f, 0.8f) * std::pow(sunDot, 64.0f) * 1.5f;
				float iblIntensity = 0.05f;
				col *= iblIntensity;

				size_t offset = (face * FACE_SIZE * FACE_SIZE + y * FACE_SIZE + x) * 4;
				mappedData[offset + 0] = col.r;
				mappedData[offset + 1] = col.g;
				mappedData[offset + 2] = col.b;
				mappedData[offset + 3] = 1.0f;
			}
		}
	}

	// create cubemap image and upload data
	uint32_t envMips = static_cast<uint32_t>(std::floor(std::log2(FACE_SIZE))) + 1;

	TextureDescription envDesc{
		.extent = {FACE_SIZE, FACE_SIZE, 1},
		.format = VK_FORMAT_R32G32B32A32_SFLOAT,
		.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.mipLevels = envMips,
		.arrayLayers = 6,
		.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
		.viewType = VK_IMAGE_VIEW_TYPE_CUBE,
		.aspectMask = static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT)
	};
	//_envCubemap = Texture("envCubemap", envDesc);
	//_envCubemap.load();
	_envCubemap = _resourceManager->load<Texture>("envCubemap", envDesc);

	// copy data from staging buffer to cubemap image and generate mips
	_vulkanCore->immediate_submit([&](VkCommandBuffer cmd) {
		vkutil::image_mem_barrier2(cmd, _envCubemap->get_image(), _envCubemap->get_format(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, 1, 0);
		std::vector<VkBufferImageCopy> copies(6);
		for (uint32_t f = 0; f < 6; ++f) {
			copies[f].bufferOffset = f * faceBytes;
			copies[f].imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, f, 1 };
			copies[f].imageExtent = _envCubemap->get_extent();
		}
		vkCmdCopyBufferToImage(cmd, staging->get_buffer(), _envCubemap->get_image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, copies.data());
		generate_cubemap_mips(cmd, _envCubemap->get_image(), FACE_SIZE, envMips, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, _envCubemap->get_format());
		});

	// cleanup staging buffer
	//staging.destroy_immediate();
	staging->unload();

	DescriptorAllocatorGrowable allocator;
	std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> poolSizes = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 10 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 10 }
	};
	allocator.init(_vulkanCore->get_device(), 20, poolSizes);

	// generate irradiance cubemap
	constexpr uint32_t IRR_SIZE = 64;
	TextureDescription irrDesc{
		.extent = { IRR_SIZE, IRR_SIZE, 1 },
		.format = VK_FORMAT_R16G16B16A16_SFLOAT,
		.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.mipLevels = 1,
		.arrayLayers = 6,
		.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
		.viewType = VK_IMAGE_VIEW_TYPE_CUBE,
		.aspectMask = static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT)
	};
	//_irradianceCubemap = Texture("irradianceCubemap", irrDesc);
	//_irradianceCubemap.load();
	_irradianceCubemap = _resourceManager->load<Texture>("irradianceCubemap", irrDesc);

	// temporary image view for compute shader
	VkImageView irrArrView;
	VkImageViewCreateInfo viewInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = _irradianceCubemap->get_image(),
		.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
		.format = VK_FORMAT_R16G16B16A16_SFLOAT,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 }
	};
	VK_CHECK(vkCreateImageView(_vulkanCore->get_device(), &viewInfo, nullptr, &irrArrView));

	VkDescriptorSet irrSet = allocator.allocate(_vulkanCore->get_device(), _irradiancePipeline.get_descriptor_set_layout(0));
	DescriptorWriter irrWriter;
	irrWriter.write_image(0, irrArrView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	irrWriter.write_image(1, _envCubemap->get_image_view(), _cubemapSampler->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	irrWriter.update_set(_vulkanCore->get_device(), irrSet);

	// run irradiance compute shader
	_vulkanCore->immediate_submit([&](VkCommandBuffer cmd) {
		vkutil::image_mem_barrier2(cmd, _irradianceCubemap->get_image(), _irradianceCubemap->get_format(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 6, 1, 0);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _irradiancePipeline.get_pipeline());
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _irradiancePipeline.get_layout(), 0, 1, &irrSet, 0, nullptr);
		vkCmdDispatch(cmd, IRR_SIZE / 16, IRR_SIZE / 16, 6);

		vkutil::image_mem_barrier2(cmd, _irradianceCubemap->get_image(), _irradianceCubemap->get_format(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 6, 1, 0);
		});
	vkDestroyImageView(_vulkanCore->get_device(), irrArrView, nullptr);

	// generate prefilter cubemap
	constexpr uint32_t PREFILTER_SIZE = 128;
	constexpr uint32_t PREFILTER_MIPS = 5;
	TextureDescription prefDesc{
		.extent = { PREFILTER_SIZE, PREFILTER_SIZE, 1 },
		.format = VK_FORMAT_R16G16B16A16_SFLOAT,
		.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.mipLevels = PREFILTER_MIPS,
		.arrayLayers = 6,
		.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
		.viewType = VK_IMAGE_VIEW_TYPE_CUBE,
		.aspectMask = static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT)
	};
	//_prefilterCubemap = Texture("prefilterCubemap", prefDesc);
	//_prefilterCubemap.load();
	_prefilterCubemap = _resourceManager->load<Texture>("prefilterCubemap", prefDesc);

	// create temporary image views for each mip level of the prefilter cubemap
	std::vector<VkImageView> prefilterMipViews(PREFILTER_MIPS);
	for (uint32_t mip = 0; mip < PREFILTER_MIPS; ++mip) {
		VkImageViewCreateInfo mipInfo{
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = _prefilterCubemap->get_image(),
			.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
			.format = VK_FORMAT_R16G16B16A16_SFLOAT,
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 6 }
		};
		VK_CHECK(vkCreateImageView(_vulkanCore->get_device(), &mipInfo, nullptr, &prefilterMipViews[mip]));
	}
	_vulkanCore->immediate_submit([&](VkCommandBuffer cmd) {
		vkutil::image_mem_barrier2(cmd, _prefilterCubemap->get_image(), _prefilterCubemap->get_format(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 6, PREFILTER_MIPS, 0);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _prefilterPipeline.get_pipeline());

		// dispatch compute shader for each mip level
		for (uint32_t mip = 0; mip < 5; ++mip) {
			uint32_t mipDim = PREFILTER_SIZE >> mip;

			VkDescriptorSet pSet = allocator.allocate(_vulkanCore->get_device(), _prefilterPipeline.get_descriptor_set_layout(0));
			DescriptorWriter pWriter;
			pWriter.write_image(0, prefilterMipViews[mip], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
			pWriter.write_image(1, _envCubemap->get_image_view(), _cubemapSampler->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			pWriter.update_set(_vulkanCore->get_device(), pSet);

			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _prefilterPipeline.get_layout(), 0, 1, &pSet, 0, nullptr);

			PrefilterPushConstants push{
				.roughness = static_cast<float>(mip) / 4.0f,
				.resolution = FACE_SIZE };
			vkCmdPushConstants(cmd, _prefilterPipeline.get_layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

			vkCmdDispatch(cmd, std::max(1u, mipDim / 16), std::max(1u, mipDim / 16), 6);
		}

		vkutil::image_mem_barrier2(cmd, _prefilterCubemap->get_image(), _prefilterCubemap->get_format(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 6, PREFILTER_MIPS, 0);
		});
	// cleanup temporary image views and descriptor pools
	for (VkImageView view : prefilterMipViews) {
		vkDestroyImageView(_vulkanCore->get_device(), view, nullptr);
	}

	// cleanup descriptor pools
	allocator.destroy_pools(_vulkanCore->get_device());
	_isHdrLoaded = false;
	_hdrPath = "Procedural sky";
}

void IBLSystem::init_samplers()
{
	VkDevice device = _vulkanCore->get_device();

	// trilinear sampler for cubemaps
	VkSamplerCreateInfo cubeInfo = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	cubeInfo.magFilter = VK_FILTER_LINEAR;
	cubeInfo.minFilter = VK_FILTER_LINEAR;
	cubeInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	cubeInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	cubeInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	cubeInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	cubeInfo.minLod = 0.0f;
	cubeInfo.maxLod = 16.0f;
	//VK_CHECK(vkCreateSampler(device, &cubeInfo, nullptr, &_cubemapSampler));
	_cubemapSampler = _resourceManager->load<Sampler>("cubemapSampler", cubeInfo);

	// linear clamped sampler for brdf lut
	VkSamplerCreateInfo lutInfo = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	lutInfo.magFilter = VK_FILTER_LINEAR;
	lutInfo.minFilter = VK_FILTER_LINEAR;
	lutInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	lutInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	lutInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	lutInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	//VK_CHECK(vkCreateSampler(device, &lutInfo, nullptr, &_lutSampler));
	_lutSampler = _resourceManager->load<Sampler>("lutSampler", lutInfo);

	// linear clamped sampler for hdr image
	VkSamplerCreateInfo hdrInfo = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	hdrInfo.magFilter = VK_FILTER_LINEAR;
	hdrInfo.minFilter = VK_FILTER_LINEAR;
	hdrInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	hdrInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	hdrInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	hdrInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	//VK_CHECK(vkCreateSampler(device, &hdrInfo, nullptr, &_hdrSampler));
	_hdrSampler = _resourceManager->load<Sampler>("hdrSampler", hdrInfo);
}

void IBLSystem::init_pipelines(ResourceManager* resourceManager)
{
	VkDevice device = _vulkanCore->get_device();

	auto eqShader = resourceManager->load<Shader>("equirect_to_cubemap.comp", VK_SHADER_STAGE_COMPUTE_BIT, "main");
	_equirectToCubemapPipeline = ComputePipeline(device, resourceManager, { .computeShader = eqShader });

	auto irrShader = resourceManager->load<Shader>("irradiance_cube.comp", VK_SHADER_STAGE_COMPUTE_BIT, "main");
	_irradiancePipeline = ComputePipeline(device, resourceManager, { .computeShader = irrShader });

	auto prefShader = resourceManager->load<Shader>("prefilter_cube.comp", VK_SHADER_STAGE_COMPUTE_BIT, "main");
	_prefilterPipeline = ComputePipeline(device, resourceManager, { .computeShader = prefShader });

	auto brdfShader = resourceManager->load<Shader>("brdf_lut.comp", VK_SHADER_STAGE_COMPUTE_BIT, "main");
	_brdfLutPipeline = ComputePipeline(device, resourceManager, { .computeShader = brdfShader });
}

void IBLSystem::generate_brdf_lut()
{
	constexpr uint32_t lutSize = 512;

	// create the brdf lut image
	TextureDescription desc{
		.extent = {lutSize, lutSize, 1},
		.format = VK_FORMAT_R16G16_SFLOAT,
		.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.mipLevels = 1,
		.arrayLayers = 1,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.aspectMask = static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT)
	};
	//_brdfLut = Texture("brdfLut", desc);
	//_brdfLut.load();
	_brdfLut = _resourceManager->load<Texture>("brdfLut", desc);

	// allocate descriptor set for brdf lut compute shader
	DescriptorAllocatorGrowable allocator;
	std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> poolSizes = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 } };
	allocator.init(_vulkanCore->get_device(), 1, poolSizes);
	VkDescriptorSet lutSet = allocator.allocate(_vulkanCore->get_device(), _brdfLutPipeline.get_descriptor_set_layout(0));

	// write the brdf lut image 
	DescriptorWriter writer;
	writer.write_image(0, _brdfLut->get_image_view(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.update_set(_vulkanCore->get_device(), lutSet);

	// dispatch the compute shader to generate the brdf lut
	_vulkanCore->immediate_submit([&](VkCommandBuffer cmd) {
		vkutil::image_mem_barrier2(cmd, _brdfLut->get_image(), _brdfLut->get_format(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 1, 1, 0);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _brdfLutPipeline.get_pipeline());
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _brdfLutPipeline.get_layout(), 0, 1, &lutSet, 0, nullptr);
		vkCmdDispatch(cmd, lutSize / 16, lutSize / 16, 1);

		vkutil::image_mem_barrier2(cmd, _brdfLut->get_image(), _brdfLut->get_format(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1, 1, 0);
		});

	// cleanup descriptor set allocator
	allocator.destroy_pools(_vulkanCore->get_device());
}

void IBLSystem::destroy_current_environment()
{
	if (_resourceManager) {
		_resourceManager->force_unload<Texture>("envCubemap");
		_resourceManager->force_unload<Texture>("irradianceCubemap");
		_resourceManager->force_unload<Texture>("prefilterCubemap");
		_resourceManager->force_unload<Texture>("hdr_img");
		_resourceManager->force_unload<Buffer>("hdr_staging");
		_resourceManager->force_unload<Buffer>("procedural_staging");
	}

	_envCubemap = {};
	_irradianceCubemap = {};
	_prefilterCubemap = {};
}

void IBLSystem::generate_cubemap_mips(VkCommandBuffer cmd, VkImage image, uint32_t size, uint32_t mipLevels, VkImageLayout layout, VkFormat format)
{
	if (mipLevels <= 1) {
		vkutil::image_mem_barrier2(cmd, image, format, layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 6, 1, 0);
		return;
	}

	uint32_t mipWidth = size;
	uint32_t mipHeight = size;

	// transition the base mip level for blitting
	vkutil::image_mem_barrier2(cmd, image, format, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 6, 1, 0);

	for (uint32_t mip = 0; mip < mipLevels - 1; mip++) {
		uint32_t nextWidth = std::max(1u, mipWidth / 2);
		uint32_t nextHeight = std::max(1u, mipHeight / 2);

		// transition next mip level to transfer dst
		vkutil::image_mem_barrier2(cmd, image, format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, 1, mip + 1);

		// blit each face of the cubemap from current mip level to next mip level
		for (uint32_t face = 0; face < 6; face++) {
			VkImageBlit2 blitRegion{ .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2 };
			blitRegion.srcOffsets[1] = { static_cast<int32_t>(mipWidth), static_cast<int32_t>(mipHeight), 1 };
			blitRegion.dstOffsets[1] = { static_cast<int32_t>(nextWidth), static_cast<int32_t>(nextHeight), 1 };

			blitRegion.srcSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.mipLevel = mip,
				.baseArrayLayer = face,
				.layerCount = 1
			};

			blitRegion.dstSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.mipLevel = mip + 1,
				.baseArrayLayer = face,
				.layerCount = 1
			};

			VkBlitImageInfo2 blitInfo{
				.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
				.srcImage = image,
				.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				.dstImage = image,
				.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				.regionCount = 1,
				.pRegions = &blitRegion,
				.filter = VK_FILTER_LINEAR
			};

			// blit image
			vkCmdBlitImage2(cmd, &blitInfo);
		}

		// transition current mip level to shaderreadonly
		vkutil::image_mem_barrier2(cmd, image, format, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 6, 1, mip);

		// prepare destination level to act as source for next step
		if (mip + 1 < mipLevels - 1)
		{
			vkutil::image_mem_barrier2(cmd, image, format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 6, 1, mip + 1);
		}
		else
		{
			// transition the final mip directly to shaderreadonly
			vkutil::image_mem_barrier2(cmd, image, format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 6, 1, mip + 1);
		}

		mipWidth = nextWidth;
		mipHeight = nextHeight;
	}
}
