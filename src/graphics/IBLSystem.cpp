#include "graphics/IBLSystem.h"

#include <graphics/VulkanCore.h>
#include <utils/vk_initializers.h>
#include <utils/vk_descriptors.h>
#include <graphics/utils/RenderSync.h>
#include <stb_image.h>
#include <fmt/core.h>
#include <cmath>

struct PrefilterPushConstants
{
	float roughness;
	float resolution;
};

void IBLSystem::init(VulkanCore* vulkanCore)
{
	_vulkanCore = vulkanCore;
	init_samplers();
	init_pipelines();
	generate_brdf_lut();
	load_procedural_default();
}

void IBLSystem::cleanup()
{
	VkDevice device = _vulkanCore->get_device();
	destroy_current_environment();

	if (_brdfLut.image != VK_NULL_HANDLE) {
		_vulkanCore->destroy_image(_brdfLut);
		_brdfLut = {};
	}
	if (_cubemapSampler != VK_NULL_HANDLE) {
		vkDestroySampler(device, _cubemapSampler, nullptr);
		_cubemapSampler = VK_NULL_HANDLE;
	}
	if (_lutSampler != VK_NULL_HANDLE) {
		vkDestroySampler(device, _lutSampler, nullptr);
		_lutSampler = VK_NULL_HANDLE;
	}
	if (_hdrSampler != VK_NULL_HANDLE) {
		vkDestroySampler(device, _hdrSampler, nullptr);
		_hdrSampler = VK_NULL_HANDLE;
	}
	if (_computeLayout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(device, _computeLayout, nullptr);
		_computeLayout = VK_NULL_HANDLE;
	}
	if (_lutComputeLayout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(device, _lutComputeLayout, nullptr);
		_lutComputeLayout = VK_NULL_HANDLE;
	}

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
	AllocatedBuffer staging = _vulkanCore->create_buffer(dataBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	memcpy(staging.info.pMappedData, data, dataBytes);

	// free image data after uploading
	stbi_image_free(data);

	// create the hdr image
	AllocatedImage hdrImage = _vulkanCore->create_image({
		.extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1},
		.format = VK_FORMAT_R32G32B32A32_SFLOAT,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.mipLevels = 1,
		.arrayLayers = 1,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
		});

	// copy image from staging buffer to hdr image
	_vulkanCore->immediate_submit([&](VkCommandBuffer cmd) {
		RenderSync::transition_image(cmd, hdrImage.image, ResourceState::Undefined, ResourceState::TransferDst);
		VkBufferImageCopy copyRegion{
			.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
			.imageExtent = hdrImage.imageExtent
		};

		vkCmdCopyBufferToImage(cmd, staging.buffer, hdrImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
		RenderSync::transition_image(cmd, hdrImage.image, ResourceState::TransferDst, ResourceState::ShaderReadOnly);
		});

	// destroy staging buffer
	_vulkanCore->destroy_buffer(staging);

	// create environment cubemap
	constexpr uint32_t ENV_CUBEMAP_SIZE = 2048;
	uint32_t envMips = static_cast<uint32_t>(std::floor(std::log2(ENV_CUBEMAP_SIZE))) + 1;

	_envCubemap = _vulkanCore->create_image({
		.extent = {ENV_CUBEMAP_SIZE, ENV_CUBEMAP_SIZE, 1},
		.format = VK_FORMAT_R16G16B16A16_SFLOAT,
		.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.mipLevels = envMips,
		.arrayLayers = 6,
		.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
		.viewType = VK_IMAGE_VIEW_TYPE_CUBE,
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
		});

	// temporary image view for compute output
	VkImageView mip0ArrayView;
	VkImageViewCreateInfo viewInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = _envCubemap.image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
		.format = _envCubemap.imageFormat,
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
	VkDescriptorSet eqSet = allocator.allocate(_vulkanCore->get_device(), _computeLayout);
	DescriptorWriter eqWriter;
	eqWriter.write_image(0, mip0ArrayView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	eqWriter.write_image(1, hdrImage.imageView, _hdrSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	eqWriter.update_set(_vulkanCore->get_device(), eqSet);

	// dispatch compute shader to convert equirectangular hdr to cubemap
	_vulkanCore->immediate_submit([&](VkCommandBuffer cmd) {
		RenderSync::transition_image(cmd, _envCubemap.image, ResourceState::Undefined, ResourceState::StorageImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _equirectToCubemapPipeline.get_pipeline());
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _equirectToCubemapPipeline.get_layout(), 0, 1, &eqSet, 0, nullptr);
		vkCmdDispatch(cmd, ENV_CUBEMAP_SIZE / 16, ENV_CUBEMAP_SIZE / 16, 6);

		// generate full mip chain for environment cubemap
		generate_cubemap_mips(cmd, _envCubemap.image, ENV_CUBEMAP_SIZE, envMips, ResourceState::StorageImage);
		});

	// cleanup temporary image view and hdr image
	vkDestroyImageView(_vulkanCore->get_device(), mip0ArrayView, nullptr);
	_vulkanCore->destroy_image(hdrImage);

	// create irradiance cubemap
	constexpr uint32_t IRR_SIZE = 64;

	_irradianceCubemap = _vulkanCore->create_image({
			.extent = { IRR_SIZE, IRR_SIZE, 1 },
			.format = VK_FORMAT_R16G16B16A16_SFLOAT,
			.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			.mipLevels = 1,
			.arrayLayers = 6,
			.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
			.viewType = VK_IMAGE_VIEW_TYPE_CUBE,
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
		});

	// create temporary image view for compute shader
	VkImageView irrArrayView;
	viewInfo.image = _irradianceCubemap.image;
	viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
	VK_CHECK(vkCreateImageView(_vulkanCore->get_device(), &viewInfo, nullptr, &irrArrayView));

	// run irradiance compute shader
	VkDescriptorSet irrSet = allocator.allocate(_vulkanCore->get_device(), _computeLayout);
	DescriptorWriter irrWriter;
	irrWriter.write_image(0, irrArrayView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	irrWriter.write_image(1, _envCubemap.imageView, _cubemapSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	irrWriter.update_set(_vulkanCore->get_device(), irrSet);

	// dispatch compute shader to generate irradiance cubemap
	_vulkanCore->immediate_submit([&](VkCommandBuffer cmd) {
		RenderSync::transition_image(cmd, _irradianceCubemap.image, ResourceState::Undefined, ResourceState::StorageImage,
			VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _irradiancePipeline.get_pipeline());
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _irradiancePipeline.get_layout(), 0, 1, &irrSet, 0, nullptr);
		vkCmdDispatch(cmd, IRR_SIZE / 16, IRR_SIZE / 16, 6);
		RenderSync::transition_image(cmd, _irradianceCubemap.image, ResourceState::StorageImage, ResourceState::ShaderReadOnly,
			VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6);
		});
	// cleanup temporary image view
	vkDestroyImageView(_vulkanCore->get_device(), irrArrayView, nullptr);

	// specular prefilter cubemap
	constexpr uint32_t PREFILTER_SIZE = 128;
	constexpr uint32_t PREFILTER_MIPS = 5;

	_prefilterCubemap = _vulkanCore->create_image({
		.extent = { PREFILTER_SIZE, PREFILTER_SIZE, 1 },
		.format = VK_FORMAT_R16G16B16A16_SFLOAT,
		.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.mipLevels = PREFILTER_MIPS,
		.arrayLayers = 6,
		.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
		.viewType = VK_IMAGE_VIEW_TYPE_CUBE,
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
		});

	// create temporary image views for each mip level of the prefilter cubemap
	std::vector<VkImageView> prefilterMipViews(PREFILTER_MIPS);
	for (uint32_t mip = 0; mip < PREFILTER_MIPS; ++mip) {
		VkImageViewCreateInfo mipViewInfo{
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.image = _prefilterCubemap.image,
				.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
				.format = _prefilterCubemap.imageFormat,
				.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 6 }
		};
		VK_CHECK(vkCreateImageView(_vulkanCore->get_device(), &mipViewInfo, nullptr, &prefilterMipViews[mip]));
	}

	// dispatch compute shader for each mip level
	_vulkanCore->immediate_submit([&](VkCommandBuffer cmd) {
		RenderSync::transition_image(cmd, _prefilterCubemap.image, ResourceState::Undefined, ResourceState::StorageImage,
			VK_IMAGE_ASPECT_COLOR_BIT, 0, PREFILTER_MIPS, 0, 6);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _prefilterPipeline.get_pipeline());

		for (uint32_t mip = 0; mip < PREFILTER_MIPS; ++mip) {
			uint32_t mipDim = PREFILTER_SIZE >> mip;

			// allocate descriptor set for this mip level
			VkDescriptorSet prefSet = allocator.allocate(_vulkanCore->get_device(), _computeLayout);
			DescriptorWriter prefWriter;
			prefWriter.write_image(0, prefilterMipViews[mip], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
			prefWriter.write_image(1, _envCubemap.imageView, _cubemapSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
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
		RenderSync::transition_image(cmd, _prefilterCubemap.image, ResourceState::StorageImage, ResourceState::ShaderReadOnly,
			VK_IMAGE_ASPECT_COLOR_BIT, 0, PREFILTER_MIPS, 0, 6);
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
	AllocatedBuffer staging = _vulkanCore->create_buffer(faceBytes * 6, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	float* mappedData = static_cast<float*>(staging.info.pMappedData);

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
	_envCubemap = _vulkanCore->create_image({
			.extent = { FACE_SIZE, FACE_SIZE, 1 },
			.format = VK_FORMAT_R32G32B32A32_SFLOAT,
			.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			.mipLevels = envMips,
			.arrayLayers = 6,
			.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
			.viewType = VK_IMAGE_VIEW_TYPE_CUBE,
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
		});

	// copy data from staging buffer to cubemap image and generate mips
	_vulkanCore->immediate_submit([&](VkCommandBuffer cmd) {
		RenderSync::transition_image(cmd, _envCubemap.image, ResourceState::Undefined, ResourceState::TransferDst,
			VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6);
		std::vector<VkBufferImageCopy> copies(6);
		for (uint32_t f = 0; f < 6; ++f) {
			copies[f].bufferOffset = f * faceBytes;
			copies[f].imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, f, 1 };
			copies[f].imageExtent = _envCubemap.imageExtent;
		}
		vkCmdCopyBufferToImage(cmd, staging.buffer, _envCubemap.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, copies.data());
		generate_cubemap_mips(cmd, _envCubemap.image, FACE_SIZE, envMips, ResourceState::TransferDst);
		});
	// cleanup staging buffer
	_vulkanCore->destroy_buffer(staging);

	DescriptorAllocatorGrowable allocator;
	std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> poolSizes = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 10 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 10 }
	};
	allocator.init(_vulkanCore->get_device(), 20, poolSizes);

	// generate irradiance cubemap
	_irradianceCubemap = _vulkanCore->create_image({
			.extent = { 32, 32, 1 },
			.format = VK_FORMAT_R16G16B16A16_SFLOAT,
			.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			.mipLevels = 1,
			.arrayLayers = 6,
			.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
			.viewType = VK_IMAGE_VIEW_TYPE_CUBE,
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
		});

	// temporary image view for compute shader
	VkImageView irrArrView;
	VkImageViewCreateInfo viewInfo{
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = _irradianceCubemap.image,
			.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
			.format = _irradianceCubemap.imageFormat,
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 }
	};
	VK_CHECK(vkCreateImageView(_vulkanCore->get_device(), &viewInfo, nullptr, &irrArrView));

	VkDescriptorSet irrSet = allocator.allocate(_vulkanCore->get_device(), _computeLayout);
	DescriptorWriter irrWriter;
	irrWriter.write_image(0, irrArrView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	irrWriter.write_image(1, _envCubemap.imageView, _cubemapSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	irrWriter.update_set(_vulkanCore->get_device(), irrSet);

	// run irradiance compute shader
	_vulkanCore->immediate_submit([&](VkCommandBuffer cmd) {
		RenderSync::transition_image(cmd, _irradianceCubemap.image, ResourceState::Undefined, ResourceState::StorageImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _irradiancePipeline.get_pipeline());
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _irradiancePipeline.get_layout(), 0, 1, &irrSet, 0, nullptr);
		vkCmdDispatch(cmd, 32 / 16, 32 / 16, 6);
		
		RenderSync::transition_image(cmd, _irradianceCubemap.image, ResourceState::StorageImage, ResourceState::ShaderReadOnly,
			VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6);
		});
	vkDestroyImageView(_vulkanCore->get_device(), irrArrView, nullptr);

	// generate prefilter cubemap
	constexpr uint32_t PREFILTER_MIPS = 5;
	_prefilterCubemap = _vulkanCore->create_image({
		.extent = { 64, 64, 1 },
		.format = VK_FORMAT_R16G16B16A16_SFLOAT,
		.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.mipLevels = PREFILTER_MIPS,
		.arrayLayers = 6,
		.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
		.viewType = VK_IMAGE_VIEW_TYPE_CUBE,
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
		});

	// create temporary image views for each mip level of the prefilter cubemap
	std::vector<VkImageView> prefilterMipViews(PREFILTER_MIPS);
	for (uint32_t mip = 0; mip < PREFILTER_MIPS; ++mip) {
		VkImageViewCreateInfo mipInfo{
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.image = _prefilterCubemap.image,
				.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
				.format = _prefilterCubemap.imageFormat,
				.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 6 }
		};
		VK_CHECK(vkCreateImageView(_vulkanCore->get_device(), &mipInfo, nullptr, &prefilterMipViews[mip]));
	}
	_vulkanCore->immediate_submit([&](VkCommandBuffer cmd) {
		RenderSync::transition_image(cmd, _prefilterCubemap.image, ResourceState::Undefined, ResourceState::StorageImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, 5, 0, 6);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _prefilterPipeline.get_pipeline());

		// dispatch compute shader for each mip level
		for (uint32_t mip = 0; mip < 5; ++mip) {
			uint32_t mipDim = 64 >> mip;

			VkDescriptorSet pSet = allocator.allocate(_vulkanCore->get_device(), _computeLayout);
			DescriptorWriter pWriter;
			pWriter.write_image(0, prefilterMipViews[mip], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
			pWriter.write_image(1, _envCubemap.imageView, _cubemapSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			pWriter.update_set(_vulkanCore->get_device(), pSet);

			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _prefilterPipeline.get_layout(), 0, 1, &pSet, 0, nullptr);

			PrefilterPushConstants push{ static_cast<float>(mip) / 4.0f, 64.0f };
			vkCmdPushConstants(cmd, _prefilterPipeline.get_layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

			vkCmdDispatch(cmd, std::max(1u, mipDim / 16), std::max(1u, mipDim / 16), 6);
		}
		
		RenderSync::transition_image(cmd, _prefilterCubemap.image, ResourceState::StorageImage, ResourceState::ShaderReadOnly, VK_IMAGE_ASPECT_COLOR_BIT, 0, 5, 0, 6);
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
	VK_CHECK(vkCreateSampler(device, &cubeInfo, nullptr, &_cubemapSampler));

	// linear clamped sampler for brdf lut
	VkSamplerCreateInfo lutInfo = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	lutInfo.magFilter = VK_FILTER_LINEAR;
	lutInfo.minFilter = VK_FILTER_LINEAR;
	lutInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	lutInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	lutInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	lutInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK_CHECK(vkCreateSampler(device, &lutInfo, nullptr, &_lutSampler));

	// linear clamped sampler for hdr image
	VkSamplerCreateInfo hdrInfo = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	hdrInfo.magFilter = VK_FILTER_LINEAR;
	hdrInfo.minFilter = VK_FILTER_LINEAR;
	hdrInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	hdrInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	hdrInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	hdrInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK_CHECK(vkCreateSampler(device, &hdrInfo, nullptr, &_hdrSampler));
}

void IBLSystem::init_pipelines()
{
	VkDevice device = _vulkanCore->get_device();

	// generic layout for storage image output + sampled input
	{
		DescriptorLayoutBuilder builder;
		builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE); // binding 0: output image
		builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // binding 1: input image
		_computeLayout = builder.build(device, VK_SHADER_STAGE_COMPUTE_BIT);
	}

	// layout for brdf lut
	{
		DescriptorLayoutBuilder builder;
		builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE); // binding 0: output image
		_lutComputeLayout = builder.build(device, VK_SHADER_STAGE_COMPUTE_BIT);
	}

	auto eqShader = Shader::load(device, VK_SHADER_STAGE_COMPUTE_BIT, "../../shaders/equirect_to_cubemap.comp.spv");
	_equirectToCubemapPipeline = ComputePipeline(device, { .computeShader = eqShader, .descriptorLayouts = { _computeLayout } });

	auto irrShader = Shader::load(device, VK_SHADER_STAGE_COMPUTE_BIT, "../../shaders/irradiance_cube.comp.spv");
	_irradiancePipeline = ComputePipeline(device, { .computeShader = irrShader, .descriptorLayouts = { _computeLayout } });

	auto prefShader = Shader::load(device, VK_SHADER_STAGE_COMPUTE_BIT, "../../shaders/prefilter_cube.comp.spv");
	_prefilterPipeline = ComputePipeline(device, {
		.computeShader = prefShader,
		.descriptorLayouts = { _computeLayout },
		.pushConstants = {{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PrefilterPushConstants) }}
		});

	auto brdfShader = Shader::load(device, VK_SHADER_STAGE_COMPUTE_BIT, "../../shaders/brdf_lut.comp.spv");
	_brdfLutPipeline = ComputePipeline(device, { .computeShader = brdfShader, .descriptorLayouts = { _lutComputeLayout } });
}

void IBLSystem::generate_brdf_lut()
{
	constexpr uint32_t lutSize = 512;

	// create the brdf lut image
	ImageDescription desc{
		.extent = {lutSize, lutSize, 1},
		.format = VK_FORMAT_R16G16_SFLOAT,
		.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.mipLevels = 1,
		.arrayLayers = 1,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT
	};
	_brdfLut = _vulkanCore->create_image(desc);

	// allocate descriptor set for brdf lut compute shader
	DescriptorAllocatorGrowable allocator;
	std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> poolSizes = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 } };
	allocator.init(_vulkanCore->get_device(), 1, poolSizes);
	VkDescriptorSet lutSet = allocator.allocate(_vulkanCore->get_device(), _lutComputeLayout);

	// write the brdf lut image 
	DescriptorWriter writer;
	writer.write_image(0, _brdfLut.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.update_set(_vulkanCore->get_device(), lutSet);

	// dispatch the compute shader to generate the brdf lut
	_vulkanCore->immediate_submit([&](VkCommandBuffer cmd) {
		RenderSync::transition_image(cmd, _brdfLut.image, ResourceState::Undefined, ResourceState::StorageImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _brdfLutPipeline.get_pipeline());
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _brdfLutPipeline.get_layout(), 0, 1, &lutSet, 0, nullptr);
		vkCmdDispatch(cmd, lutSize / 16, lutSize / 16, 1);

		RenderSync::transition_image(cmd, _brdfLut.image, ResourceState::StorageImage, ResourceState::ShaderReadOnly, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
		});

	// cleanup descriptor set allocator
	allocator.destroy_pools(_vulkanCore->get_device());
}

void IBLSystem::destroy_current_environment()
{
	if (_envCubemap.image != VK_NULL_HANDLE) {
		_vulkanCore->destroy_image(_envCubemap);
		_envCubemap = {};
	}

	if (_irradianceCubemap.image != VK_NULL_HANDLE) {
		_vulkanCore->destroy_image(_irradianceCubemap);
		_irradianceCubemap = {};
	}

	if (_prefilterCubemap.image != VK_NULL_HANDLE) {
		_vulkanCore->destroy_image(_prefilterCubemap);
		_prefilterCubemap = {};
	}
}

void IBLSystem::generate_cubemap_mips(VkCommandBuffer cmd, VkImage image, uint32_t size, uint32_t mipLevels, ResourceState baseState)
{
	if (mipLevels <= 1) {
		RenderSync::transition_image(cmd, image, baseState, ResourceState::ShaderReadOnly, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6);
		return;
	}

	uint32_t mipWidth = size;
	uint32_t mipHeight = size;

	// transition the base mip level for blitting
	RenderSync::transition_image(cmd, image, baseState, ResourceState::TransferSrc, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6);

	for (uint32_t mip = 0; mip < mipLevels - 1; mip++) {
		uint32_t nextWidth = std::max(1u, mipWidth / 2);
		uint32_t nextHeight = std::max(1u, mipHeight / 2);

		// transition next mip level to transfer dst
		RenderSync::transition_image(cmd, image, ResourceState::Undefined, ResourceState::TransferDst, VK_IMAGE_ASPECT_COLOR_BIT, mip + 1, 1, 0, 6);

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
		RenderSync::transition_image(cmd, image, ResourceState::TransferSrc, ResourceState::ShaderReadOnly, VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 6);

		// prepare destination level to act as source for next step
		if (mip + 1 < mipLevels - 1)
		{
			RenderSync::transition_image(cmd, image, ResourceState::TransferDst, ResourceState::TransferSrc, VK_IMAGE_ASPECT_COLOR_BIT, mip + 1, 1, 0, 6);
		}
		else
		{
			// transition the final mip directly to shaderreadonly
			RenderSync::transition_image(cmd, image, ResourceState::TransferDst, ResourceState::ShaderReadOnly, VK_IMAGE_ASPECT_COLOR_BIT, mip + 1, 1, 0, 6);
		}

		mipWidth = nextWidth;
		mipHeight = nextHeight;
	}
}
