#pragma once
 
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <vector>
#include <unordered_map>

struct PooledImage
{
	VkImage image{ VK_NULL_HANDLE };
	VkImageView imageView{ VK_NULL_HANDLE };
	std::vector<VkImageView> faceViews;
	VmaAllocation allocation{ VK_NULL_HANDLE };
	uint32_t lastUsedFrame{ 0 };
	bool inUse{ false };
};

struct AllocationKey
{
	VkExtent3D extent;
	VkFormat format;
	VkImageUsageFlags usage;
	VkImageCreateFlags flags;
	VkImageViewType viewType;
	uint32_t mipLevels;
	uint32_t arrayLayers;

	bool operator==(const AllocationKey& other) const
	{
		return extent.width == other.extent.width && 
					 extent.height == other.extent.height && 
					 extent.depth == other.extent.depth &&
					 format == other.format &&
					 usage == other.usage &&
					 flags == other.flags &&
					 viewType == other.viewType &&
					 mipLevels == other.mipLevels &&
					 arrayLayers == other.arrayLayers;
	}
};

struct AllocationKeyHash // hash function for AllocationKey
{
	size_t operator()(const AllocationKey& key) const // hash function for AllocationKey
	{
		size_t h = std::hash<uint32_t>()(key.extent.width) ^ (std::hash<uint32_t>()(key.extent.height) << 1); // height shift left by 1
		h ^= std::hash<uint32_t>()(key.format) << 2; // format shift left by2 
		h ^= std::hash<uint32_t>()(key.usage) << 3; // usage shift left by 3
		h ^= std::hash<uint32_t>()(key.flags) << 4; // flags shift left by 4
		return h;
	}
};

class RenderTargetPool
{
public:
	void init(VkDevice device, VmaAllocator allocator);
	void cleanup();
	void reset_frame(uint32_t currentFrame);

	PooledImage* get_image(const AllocationKey& key, uint32_t currentFrame); // get or create a pooled image based given key

private:
	VkDevice _device{ VK_NULL_HANDLE };
	VmaAllocator _allocator{ VK_NULL_HANDLE };
	std::unordered_map<AllocationKey, std::vector<PooledImage>, AllocationKeyHash> _pool;
};