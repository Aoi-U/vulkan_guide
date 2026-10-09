#pragma once

#include <graphics/utils/vk_types.h>
#include <filesystem>
#include <resources/ResourceManager.h>
#include <string>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

class Texture : public Resource
{
public:
	Texture() 
		: Resource("", nullptr, nullptr), _format(VK_FORMAT_UNDEFINED), _usage(0), _mipmapped(false) {}

	// construct texture from file path
	explicit Texture(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, const std::filesystem::path& filePath = "", VkFormat format = VK_FORMAT_R8G8B8A8_UNORM, VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT, bool generateMips = true);

	// construct texture from embedded data
	explicit Texture(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, std::span<const uint8_t> data, VkFormat format = VK_FORMAT_R8G8B8A8_UNORM, VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT, bool generateMips = true);

	// construct texture from description and optional data
	explicit Texture(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, const TextureDescription& desc, std::span<const uint8_t> data = {});

	~Texture() override { Resource::unload(); }

	void destroy_immediate();

	VkImage get_image() const { return _image; }
	VkImageView get_image_view() const { return _imageView; }
	VkImageView get_face_view(uint32_t faceIndex) const { return (faceIndex < _faceViews.size()) ? _faceViews[faceIndex] : _imageView; }
	const std::vector<VkImageView>& get_face_views() const { return _faceViews; }
	VkSampler get_sampler() const { return _sampler; }
	VkExtent3D get_extent() const { return _extent; }
	VkExtent2D get_extent_2d() const { return { _extent.width, _extent.height }; }
	VkFormat get_format() const { return _format; }
	uint32_t get_bindless_index() const { return _bindlessIndex; }

protected:
	bool doLoad() override;
	bool doUnload() override;

private:
	enum class SourceType { File, Encoded, Description };
	SourceType _sourceType{ SourceType::File };

	std::filesystem::path _filePath;
	std::vector<uint8_t> _bufferData; 
	TextureDescription _desc;

	VkFormat _format;
	VkImageUsageFlags _usage;
	bool _mipmapped;

	VkImage _image{ nullptr };
	VmaAllocation _allocation{ nullptr };
	VkImageView _imageView{ nullptr };
	VkSampler _sampler{ nullptr };
	VkExtent3D _extent{ 1, 1, 1 };
	uint32_t _mipLevels{ 0 };
	std::vector<VkImageView> _faceViews; // for cubemaps

	uint32_t _bindlessIndex{ 0 }; // index in the bindless descriptor set 
};