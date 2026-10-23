#pragma once

#include <graphics/vt/VirtualTextureTypes.h>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <vector>
#include <unordered_map>
#include <optional>
#include <span>
#include <mutex>

class VulkanCore;
class StagingUploader;

// manages fixed size gpu physical tile cache texture
class PhysicalTilePool
{
public:
	void init(VulkanCore* core,
		StagingUploader* uploader,
		uint32_t tilesPerAxis = VT_DEFAULT_ATLAS_TILES_PER_AXIS,
		uint32_t layerCount = static_cast<uint32_t>(VTPhysicalLayer::Count),
		VkFormat format = VK_FORMAT_R8G8B8A8_UNORM
	);
	
	void cleanup();

	// finds existing slot for pageId or allocates free/LRU slot
	bool request_or_allocate_slot(const VirtualPageID& pageId, 
		uint64_t currentFrame, 
		bool pinSlot, 
		PhysicalSlotCoord& outSlot, 
		std::optional<VirtualPageID>& outEvictedPage, 
		bool& outWasCacheHit
	);

	// promotes resident slot to MRU head of LRU list
	void touch_slot(uint32_t slotIndex, uint64_t currentFrame);

	// mark slot pinned
	void set_slot_pinned(uint32_t slotIndex, bool pinned);

	// free all physical slots belonging to a given virtual texture id
	void free_slots_for_texture(uint32_t textureId, std::vector<VirtualPageID>& outFreedPages);

	// find if virtual page is resident in physical pool
	std::optional<PhysicalSlotCoord> find_resident_page(const VirtualPageID& pageId) const;

	// stage 136x136 tile upload into atlas
	bool stage_tile_upload(uint32_t slotIndex, uint32_t layerIndex, std::span<const uint8_t> tilePixelsWithBorder);

	// getters
	VkImage get_atlas_image() const noexcept { return _atlasImage; }
	VkImageView get_atlas_view() const noexcept { return _atlasImageView; }
	VkImageView get_layer_view(uint32_t layer) const noexcept { return (layer < _layerViews.size()) ? _layerViews[layer] : _atlasImageView; }
	VkSampler get_atlas_sampler() const noexcept { return _atlasSampler; }
	VkFormat get_format() const noexcept { return _format; }
	VkExtent3D get_atlas_extent() const noexcept { return _atlasExtent; }

	uint32_t get_tiles_per_axis() const noexcept { return _tilesPerAxis; }
	uint32_t get_total_slots() const noexcept { return _totalSlots; }
	uint32_t get_used_slots() const noexcept { return _usedSlots; }
	uint32_t get_pinned_slots() const noexcept { return _pinnedSlots; }
	uint64_t get_vram_bytes() const noexcept { return _vramBytes; }
	uint64_t get_cache_hits() const noexcept { return _cacheHits; }
	uint64_t get_cache_misses() const noexcept { return _cacheMisses; }
	uint64_t get_evictions() const noexcept { return _evictions; }

private:
	struct SlotNode
	{
		uint32_t slotIndex{ 0 };
		uint8_t tileX{ 0 };
		uint8_t tileY{ 0 };
		bool inUse{ false };
		bool pinned{ false };
		uint64_t lastUsedFrame{ 0 };
		VirtualPageID residentPage{};
		
		uint32_t prevLru{ VT_INVALID_PHYSICAL_SLOT };
		uint32_t nextLru{ VT_INVALID_PHYSICAL_SLOT };
	};

	void lru_remove(uint32_t slotIndex);
	void lru_push_front(uint32_t slotIndex);

	VulkanCore* _core{ nullptr };
	StagingUploader* _uploader{ nullptr };

	uint32_t _tilesPerAxis{ VT_DEFAULT_ATLAS_TILES_PER_AXIS };
	uint32_t _totalSlots{ 0 };
	uint32_t _layerCount{ 1 };
	VkFormat _format{ VK_FORMAT_R8G8B8A8_UNORM };
	VkExtent3D _atlasExtent{ 1, 1,1 };
	uint64_t _vramBytes{ 0 };

	VkImage _atlasImage{ VK_NULL_HANDLE };
	VmaAllocation _atlasAllocation{ VK_NULL_HANDLE };
	VkImageView _atlasImageView{ VK_NULL_HANDLE };
	std::vector<VkImageView> _layerViews;
	VkSampler _atlasSampler{ VK_NULL_HANDLE };

	mutable std::mutex _mutex;
	std::vector<SlotNode> _slots;
	std::vector<uint32_t> _freeSlotStack;
	std::unordered_map<VirtualPageID, uint32_t> _pageToSlot;

	uint32_t _lruHead{ VT_INVALID_PHYSICAL_SLOT };
	uint32_t _lruTail{ VT_INVALID_PHYSICAL_SLOT };

	uint32_t _usedSlots{ 0 };
	uint32_t _pinnedSlots{ 0 };
	uint64_t _cacheHits{ 0 };
	uint64_t _cacheMisses{ 0 };
	uint64_t _evictions{ 0 };
};