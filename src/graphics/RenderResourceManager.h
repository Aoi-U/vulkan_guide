#pragma once

#include <core/vk_types.h>
#include <graphics/utils/RenderSync.h>
#include <unordered_map>
#include <string>
#include <vector>
#include <span>

class VulkanCore;

enum class TargetSizePolicy {
	WindowRelative, // size relative to window size, scales when window resizes
	Absolute // fixed size
};

// description of a render target 
struct RenderTargetDescription
{
	std::string name;
	TargetSizePolicy sizePolicy{ TargetSizePolicy::WindowRelative };
	float relativeScale{ 1.0f };
	VkExtent2D absoluteExtent{ 0, 0 };
	VkFormat format{ VK_FORMAT_R16G16B16A16_SFLOAT };
	VkImageUsageFlags usage{ VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT };
	uint32_t mipLevels{ 1 };
	uint32_t arrayLayers{ 1 };
	VkImageCreateFlags flags;
	VkImageViewType viewType{ VK_IMAGE_VIEW_TYPE_2D };
	VkImageAspectFlags aspectMask{ VK_IMAGE_ASPECT_COLOR_BIT };
};

struct RenderTarget
{
	RenderTargetDescription description;
	AllocatedImage image;
	ResourceState currentState{ ResourceState::Undefined };
	std::vector<VkImageView> faceViews; // used for cube maps
};

class RenderResourceManager
{
public:
	void init(VulkanCore* vulkanCore);
	void cleanup();

	// add a render target to the manager
	void register_target(const RenderTargetDescription& desc, VkExtent2D currentWindowExtent);

	// resize all window relative render targets
	void on_window_resize(VkExtent2D newWindowExtent);

	// transition a render target to a new state
	void transition(VkCommandBuffer cmd, const std::string& name, ResourceState newState);

	// set the state of a render target 
	void set_state(const std::string& name, ResourceState state);

	// getters
	AllocatedImage& get_image(const std::string& name);
	VkImageView get_view(const std::string& name);
	VkExtent2D get_extent(const std::string& name) const;
	VkFormat get_format(const std::string& name) const;
	ResourceState get_state(const std::string& name) const;
	VkImageView get_face_view(const std::string& name, uint32_t face) const { return _targets.at(name).faceViews.at(face); }
	const RenderTargetDescription& get_description(const std::string& name) const { return _targets.at(name).description; }
	bool has_target(const std::string& name) const;

private:
	void allocate_target_resources(RenderTarget& target, VkExtent2D windowExtent);
	void destroy_target_resources(RenderTarget& target);

	VulkanCore* _vulkanCore;
	VkExtent2D _lastWindowExtent{ 0, 0 };
	std::unordered_map<std::string, RenderTarget> _targets;
};
