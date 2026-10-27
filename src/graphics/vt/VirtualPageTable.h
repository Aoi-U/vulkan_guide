#pragma once

#include <graphics/vt/VirtualTextureTypes.h>
#include <offsetAllocator.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <vector>
#include <unordered_map>
#include <mutex>

class VulkanCore;
class StagingUploader;

// allocation record for a registered virtual texture inside global page table
struct PageTableAllocation
{
	uint16_t textureId{ VT_INVALID_TEXTURE_ID };
	uint32_t offsetX{ 0 };
	uint32_t offsetY{ 0 };
	uint32_t tilesX{ 0 };
	uint32_t tilesY{ 0 };
	uint32_t mipLevels{ 1 };
	OffsetAllocator::Allocation regionAlloc{};

	bool is_valid() const noexcept { return textureId != VT_INVALID_TEXTURE_ID; }
};

// table of virtual texture allocations
class VirtualPageTable
{
public:
	static constexpr uint32_t DIRTY_GRID_DIM = 16;

	void init(VulkanCore* core, StagingUploader* uploader, uint32_t pageTableSizeMip0 = VT_DEFAULT_PAGE_TABLE_SIZE, uint32_t maxMipLevels = VT_MAX_MIP_LEVELS);
	void cleanup();

	// suballocate virtual tile region for a texture
	PageTableAllocation allocate_virtual_range(uint16_t textureId, uint32_t virtualWidth, uint32_t virtualHeight, uint32_t mipLevels);

	// free allocated virtual tile range  
	void free_virtual_range(const PageTableAllocation& alloc);

	// maps virtual page to physical slot
	void map_page(const PageTableAllocation& alloc, uint16_t localTileX, uint16_t localTileY, uint8_t mipLevel, uint8_t physTileX, uint8_t physTileY);

	// unmap evicted virtual page 
	void unmap_page(const PageTableAllocation& alloc, uint16_t localTileX, uint16_t localTileY, uint8_t mipLevel);

	// stage all dirty page table rectangles across all mip levels to gpu image
	uint32_t flush_dirty_pages();

	// read cpu shadow page table entry for testing
	PageTableEntry get_cpu_entry(uint32_t globalTileX, uint32_t globalTileY, uint8_t mipLevel) const;

	// getters
	VkImage get_image() const noexcept { return _pageTableImage; }
	VkImageView get_image_view() const noexcept { return _pageTableView; }
	VkSampler get_sampler() const noexcept { return _nearestSampler; }
	uint32_t get_size_mip0() const noexcept { return _sizeMip0; }
	uint32_t get_mip_count() const noexcept { return _mipCount; }
	uint64_t get_vram_bytes() const noexcept { return _vramBytes; }
	uint32_t get_allocated_tiles_mip0() const noexcept { return _allocatedTilesMip0; }

private:
	struct DirtyRect
	{
		uint32_t minX{ UINT32_MAX };
		uint32_t minY{ UINT32_MAX };
		uint32_t maxX{ 0 }; // inclusive
		uint32_t maxY{ 0 }; // inclusive

		bool is_dirty() const noexcept { return minX <= maxX && minY <= maxY; }
		void reset() noexcept
		{
			minX = UINT32_MAX;
			minY = UINT32_MAX;
			maxX = 0;
			maxY = 0;
		}

		void expand(uint32_t x, uint32_t y) noexcept
		{
			minX = std::min(minX, x);
			minY = std::min(minY, y);
			maxX = std::max(maxX, x);
			maxY = std::max(maxY, y);
		}
	};

	struct MipLevelStorage
	{
		uint32_t width{ 1 };
		uint32_t height{ 1 };
		uint32_t sectorSpan{ 1 };
		uint32_t sectorsPerAxis{ 1 };
		std::vector<PageTableEntry> entries; // exact or inherited fallback entries sent to gpu
		std::vector<PageTableEntry> exactEntries; // only populated if this exact mip is resident
		std::vector<DirtyRect> sectorDirtyRects;
		bool anyDirty{ false };
	};

	void write_entry(uint8_t mip, uint32_t x, uint32_t y, PageTableEntry entry);
	void propagate_quadtree_children(const PageTableAllocation& alloc, uint16_t parentLocalX, uint16_t parentLocalY, uint8_t parentMip, PageTableEntry fallbackEntry);

	VulkanCore* _core{ nullptr };
	StagingUploader* _uploader{ nullptr };

	uint32_t _sizeMip0{ VT_DEFAULT_PAGE_TABLE_SIZE };
	uint32_t _mipCount{ VT_MAX_MIP_LEVELS };
	uint32_t _macroBlockSize{ 16 };
	uint32_t _macroGridWidth{ 64 };
	uint32_t _allocatedTilesMip0{ 0 };
	uint64_t _vramBytes{ 0 };

	VkImage _pageTableImage{ VK_NULL_HANDLE };
	VmaAllocation _pageTableAlloc{ VK_NULL_HANDLE };
	VkImageView _pageTableView{ VK_NULL_HANDLE };
	VkSampler _nearestSampler{ VK_NULL_HANDLE };

	mutable std::mutex _mutex;
	std::unique_ptr<OffsetAllocator::Allocator> _virtualSpaceAllocator;
	std::vector<MipLevelStorage> _mips;
};