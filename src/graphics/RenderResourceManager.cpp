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

void RenderResourceManager::transition(VkCommandBuffer cmd, const std::string& name, ResourceState newState)
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
	RenderSync::transition_image(cmd, target.image.get_image(), target.currentState, newState, target.description.aspectMask, 0, target.description.mipLevels, 0, target.description.arrayLayers);
	target.currentState = newState;
}

void RenderResourceManager::transition_all_to_read_only(VkCommandBuffer cmd)
{
	std::vector<ImageBarrierInfo> barriers;
	for (auto& [name, target] : _targets) {
		// skip targets already in read only or undefined
		if (target.currentState == ResourceState::ShaderReadOnly || target.currentState == ResourceState::Undefined) {
			continue;
		}

		ResourceState targetState = ResourceState::ShaderReadOnly;
		const RenderTargetDescription& desc = target.description;

		ImageBarrierInfo barrier{};
		barrier.image = target.image.get_image();
		barrier.oldState = target.currentState;
		barrier.newState = targetState;
		barrier.aspectMask = desc.aspectMask;
		barrier.baseMipLevel = 0;
		barrier.levelCount = desc.mipLevels;
		barrier.baseArrayLayer = 0;
		barrier.layerCount = desc.arrayLayers;
		barriers.push_back(barrier);
		target.currentState = targetState;
	}

	// transition all images to readonly
	if (!barriers.empty()) {
		RenderSync::transition_images(cmd, barriers);
	}
}

void RenderResourceManager::set_state(const std::string& name, ResourceState state)
{
	auto it = _targets.find(name);
	if (it != _targets.end()) {
		it->second.currentState = state;
	}
}

Texture& RenderResourceManager::get_image(const std::string & name)
{
	return _targets.at(name).image;
}

VkImageView RenderResourceManager::get_view(const std::string& name)
{
	return _targets.at(name).image.get_image_view();
}

VkExtent2D RenderResourceManager::get_extent(const std::string& name) const
{
	const Texture& img = _targets.at(name).image;
	return img.get_extent_2d();
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

std::vector<std::string> RenderResourceManager::get_target_names() const
{
	std::vector<std::string> names;
	names.reserve(_targets.size());
	for (const auto& [name, target] : _targets) {
		names.push_back(name);
	}

	std::sort(names.begin(), names.end());
	return names;
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
	TextureDescription desc;
	desc.extent = extent;
	desc.format = target.description.format;
	desc.usage = target.description.usage;
	desc.mipLevels = target.description.mipLevels;
	desc.arrayLayers = target.description.arrayLayers;
	desc.samples = VK_SAMPLE_COUNT_1_BIT;
	desc.flags = target.description.flags;
	desc.viewType = target.description.viewType;
	desc.aspectMask = target.description.aspectMask;

	target.image = Texture(target.description.name + "_tex", desc);
	target.image.load(_vulkanCore);
	target.currentState = ResourceState::Undefined;

	// create 2d face views for cube maps
	if ((target.description.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) || (target.description.arrayLayers == 6 && target.description.viewType == VK_IMAGE_VIEW_TYPE_CUBE)) {
		target.faceViews.resize(6, VK_NULL_HANDLE);
		for (uint32_t i = 0; i < 6; i++) {
			VkImageViewCreateInfo viewInfo{ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
			viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			viewInfo.image = target.image.get_image();
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
	target.image.unload(_vulkanCore);
}
