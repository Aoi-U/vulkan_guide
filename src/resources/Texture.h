#pragma once

#include <resources/ResourceManager.h>
#include <core/vk_types.h>
#include <filesystem>
#include <string>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

class Texture : public Resource
{
public:
	explicit Texture(const std::string& id, const std::filesystem::path& filePath = "", VkFormat format = VK_FORMAT_R8G8B8A8_UNORM, VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT, bool generateMips = true);

	explicit Texture(const std::string& id, std::span<const uint8_t> data, VkFormat format = VK_FORMAT_R8G8B8A8_UNORM, VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT, bool generateMips = true);

	~Texture() override = default;

	VkImage get_image() const { return _image; }
	VkImageView get_image_view() const { return _imageView; }
	VkSampler get_sampler() const { return _sampler; }
	VkExtent3D get_extent() const { return _extent; }
	VkFormat get_format() const { return _format; }

protected:
	bool doLoad(VulkanCore* core) override;
	bool doUnload(VulkanCore* core) override;

private:
	std::filesystem::path _filePath;
	std::vector<uint8_t> _embeddedData; 
	VkFormat _format;
	VkImageUsageFlags _usage;
	bool _mipmapped;

	VkImage _image{ nullptr };
	VmaAllocation _allocation{ nullptr };
	VkImageView _imageView{ nullptr };
	VkSampler _sampler{ nullptr };
	VkExtent3D _extent{ 1, 1, 1 };
	uint32_t _mipLevels;

};