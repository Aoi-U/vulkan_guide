#include "RenderResourceManager.h"
#include "VulkanCore.h"
#include <algorithm>
#include <glm/gtc/packing.hpp>

void RenderResourceManager::init(VulkanCore* vulkanCore)
{
	_vulkanCore = vulkanCore;
}

void RenderResourceManager::cleanup()
{
	for (auto& [name, target] : _targets) {
		destroy_target_resources(target);
	}

	_targets.clear();
	_vulkanCore = nullptr;
}

void RenderResourceManager::register_target(const RenderTargetDescription& desc, VkExtent2D currentWindowExtent)
{
	// destroy target if target already exists
	if (_targets.contains(desc.name)) {
		destroy_target_resources(_targets[desc.name]);
	}

	// create new target
	RenderTarget target;
	target.description = desc;
	_lastWindowExtent = currentWindowExtent;
	allocate_target_resources(target, currentWindowExtent);
	_targets[desc.name] = target;
}

void RenderResourceManager::on_window_resize(VkExtent2D newWindowExtent)
{
	_lastWindowExtent = newWindowExtent;
	for (auto& [name, target] : _targets) {
		if (target.description.sizePolicy == TargetSizePolicy::WindowRelative) {
			destroy_target_resources(target);
			allocate_target_resources(target, newWindowExtent);
		}
	}
}

void RenderResourceManager::transition(VkCommandBuffer cmd, const std::string name, ResourceState newState)
{
	auto it = _targets.find(name);
	if (it == _targets.end()) {
		fmt::println("transition failed: target '{}' not found", name);
		return;
	}

	RenderTarget& target = it->second;
	// no transition needed if newState is same as current state
	if (target.currentState == newState)
		return;

	// transition image to the new state
	RenderSync::transition_image(cmd, target.image.image, target.currentState, newState, target.description.aspectMask, 0, target.description.mipLevels, 0, target.description.arrayLayers);
	target.currentState = newState;
}

void RenderResourceManager::transition_all(VkCommandBuffer cmd, std::span<const std::pair<std::string, ResourceState>> transitions)
{
	std::vector<ImageBarrierInfo> barriers;
	barriers.reserve(transitions.size());

	// create barriers for all transitions
	for (const auto& [name, targetState] : transitions) {
		auto it = _targets.find(name);
		if (it == _targets.end()) {
			fmt::println("transition failed: target '{}' not found", name);
			continue;
		}

		RenderTarget& target = it->second;
		if (target.currentState == targetState)
			continue;

		barriers.push_back({ target.image.image, target.currentState, targetState, target.description.aspectMask, 0, target.description.mipLevels, 0, target.description.arrayLayers });
		target.currentState = targetState;
	}

	// transition all images
	RenderSync::transition_images(cmd, barriers);
}

void RenderResourceManager::generate_procedural_cubemap(const std::string& name)
{
	auto it = _targets.find(name);
	if (it == _targets.end()) return;

	RenderTarget& target = it->second;
	const uint32_t faceSize = target.image.imageExtent.width;
	const size_t faceBytes = faceSize * faceSize * 4;
	const size_t totalBytes = faceBytes * 6;

	AllocatedBuffer staging = _vulkanCore->create_buffer(totalBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);

	uint8_t* mappedData = static_cast<uint8_t*>(staging.info.pMappedData);

	// set colors for sky and ground
	const glm::vec3 zenith(0.18f, 0.38f, 0.85f);
	const glm::vec3 horizon(0.72f, 0.82f, 0.95f);
	const glm::vec3 ground(0.22f, 0.22f, 0.25f);
	const glm::vec3 sunDir = glm::normalize(glm::vec3(0.4f, 1.0f, 0.3f));

	for (uint32_t face = 0; face < 6; ++face) {
		for (uint32_t y = 0; y < faceSize; ++y) {
			for (uint32_t x = 0; x < faceSize; ++x) {
				float u = (2.0f * (x + 0.5f) / faceSize) - 1.0f;
				float v = (2.0f * (y + 0.5f) / faceSize) - 1.0f;

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

				glm::vec3 color = (dir.y > 0.0f) ? glm::mix(horizon, zenith, dir.y)
					: glm::mix(horizon, ground, -dir.y);
				float sunDot = glm::max(glm::dot(dir, sunDir), 0.0f);
				color += glm::vec3(1.0f, 0.95f, 0.8f) * std::pow(sunDot, 64.0f) * 1.5f;

				size_t offset = (face * faceBytes) + (y * faceSize + x) * 4;
				mappedData[offset + 0] = static_cast<uint8_t>(std::clamp(color.r, 0.0f, 1.0f) * 255.0f);
				mappedData[offset + 1] = static_cast<uint8_t>(std::clamp(color.g, 0.0f, 1.0f) * 255.0f);
				mappedData[offset + 2] = static_cast<uint8_t>(std::clamp(color.b, 0.0f, 1.0f) * 255.0f);
				mappedData[offset + 3] = 255;
			}
		}
	}

	// copy the staging buffer to the cubemap image
	_vulkanCore->immediate_submit([&](VkCommandBuffer cmd) {
		RenderSync::transition_image(cmd, target.image.image, ResourceState::Undefined, ResourceState::TransferDst,
			VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6);

		std::vector<VkBufferImageCopy> copyRegions(6);
		for (uint32_t f = 0; f < 6; ++f) {
			copyRegions[f].bufferOffset = f * faceBytes;
			copyRegions[f].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			copyRegions[f].imageSubresource.mipLevel = 0;
			copyRegions[f].imageSubresource.baseArrayLayer = f;
			copyRegions[f].imageSubresource.layerCount = 1;
			copyRegions[f].imageExtent = target.image.imageExtent;
		}

		vkCmdCopyBufferToImage(cmd, staging.buffer, target.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(copyRegions.size()), copyRegions.data());

		RenderSync::transition_image(cmd, target.image.image, ResourceState::TransferDst, ResourceState::ShaderReadOnly, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6);});

	target.currentState = ResourceState::ShaderReadOnly;

	// destroy the staging buffer
	_vulkanCore->destroy_buffer(staging);
}

AllocatedImage& RenderResourceManager::get_image(const std::string & name)
{
	return _targets.at(name).image;
}

VkImageView RenderResourceManager::get_view(const std::string& name)
{
	return _targets.at(name).image.imageView;
}

VkImageView RenderResourceManager::get_face_view(const std::string& name, uint32_t faceIndex)
{
	return _targets.at(name).faceViews.at(faceIndex);
}

VkExtent2D RenderResourceManager::get_extent(const std::string& name) const
{
	const AllocatedImage& img = _targets.at(name).image;
	return { img.imageExtent.width, img.imageExtent.height };
}

VkFormat RenderResourceManager::get_format(const std::string& name) const
{
	return _targets.at(name).description.format;
}

ResourceState RenderResourceManager::get_state(const std::string& name) const
{
	return _targets.at(name).currentState;
}

bool RenderResourceManager::has_target(const std::string& name) const
{
	return _targets.contains(name);
}

void RenderResourceManager::allocate_target_resources(RenderTarget& target, VkExtent2D windowExtent)
{
	// set the target extent based on size policy
	VkExtent3D extent;
	if (target.description.sizePolicy == TargetSizePolicy::WindowRelative) {
		extent.width = std::max(1u, static_cast<uint32_t>(windowExtent.width * target.description.relativeScale));
		extent.height = std::max(1u, static_cast<uint32_t>(windowExtent.height * target.description.relativeScale));
		extent.depth = 1;
	}
	else {
		extent.width = std::max(1u, target.description.absoluteExtent.width);
		extent.height = std::max(1u, target.description.absoluteExtent.height);
		extent.depth = 1;
	}

	// create the target image
	ImageDescription desc;
	desc.extent = extent;
	desc.format = target.description.format;
	desc.usage = target.description.usage;
	desc.mipLevels = target.description.mipLevels;
	desc.arrayLayers = target.description.arrayLayers;
	desc.samples = VK_SAMPLE_COUNT_1_BIT;
	desc.flags = target.description.flags;
	desc.viewType = target.description.viewType;
	desc.aspectMask = target.description.aspectMask;

	target.image = _vulkanCore->create_image(desc);
	target.currentState = ResourceState::Undefined;

	// create 2d face views for cube maps
	if ((target.description.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) || (target.description.arrayLayers == 6 && target.description.viewType == VK_IMAGE_VIEW_TYPE_CUBE)) {
		target.faceViews.resize(6, VK_NULL_HANDLE);
		for (uint32_t i = 0; i < 6; i++) {
			VkImageViewCreateInfo viewInfo{ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
			viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			viewInfo.image = target.image.image;
			viewInfo.format = target.description.format;
			viewInfo.subresourceRange.aspectMask = target.description.aspectMask;
			viewInfo.subresourceRange.baseMipLevel = 0;
			viewInfo.subresourceRange.levelCount = 1;
			viewInfo.subresourceRange.baseArrayLayer = i;
			viewInfo.subresourceRange.layerCount = 1;

			VK_CHECK(vkCreateImageView(_vulkanCore->get_device(), &viewInfo, nullptr, &target.faceViews[i]));
		}
	}
}

void RenderResourceManager::destroy_target_resources(RenderTarget & target)
{
	// destroy image views
	for (VkImageView view : target.faceViews) {
		if (view != VK_NULL_HANDLE) {
			vkDestroyImageView(_vulkanCore->get_device(), view, nullptr);
		}
	}
	target.faceViews.clear();

	// destroy image
	if (target.image.image != VK_NULL_HANDLE) {
		_vulkanCore->destroy_image(target.image);
		target.image = {};
	}
}
