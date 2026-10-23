#pragma once

#include <graphics/vt/VirtualTextureTypes.h>
#include <graphics/vt/PhysicalTilePool.h>
#include <graphics/vt/VirtualPageTable.h>
#include <resources/Buffer.h>
#include <core/UUID.h>
#include <vector>
#include <unordered_map>
#include <span>
#include <mutex>

class VulkanCore;
class ResourceManager;

// system managing virtual textures, including physical tile pool, virtual page table, and related resources
class VirtualTextureSystem
{
public:
	static constexpr uint32_t MAX_VIRTUAL_TEXTURES = 4096;

	void init(VulkanCore* core, ResourceManager* rm, uint32_t atlasTilesPerAxis = VT_DEFAULT_ATLAS_TILES_PER_AXIS, uint32_t pageTableSize = VT_DEFAULT_PAGE_TABLE_SIZE);

	void cleanup();

	// register a virtual texture in the page table, and pins its 1x1 tail mip in  the physical pool
	// return its runtime virtual texture id
	uint16_t register_virtual_texture(const UUID& assetUuid, uint32_t virtualWidth, uint32_t virtualHeight, uint32_t mipLevels, std::span<const uint8_t> tailMipAlbedoPixels = {});

	// unregisters a virtual texture
	// frees its physical tiles and page table allocation
	void unregister_virtual_texture(uint16_t textureId);

	// map streamed tile into physical pool and update virtual page table
	bool map_streamed_tile(const VirtualPageID& pageId, uint64_t currentFrame, bool pinTile,
		std::span<const uint8_t> albedoPixels,
		std::span<const uint8_t> normalPixels = {},
		std::span<const uint8_t> metalRoughPixels = {}
	);

	// flush dirty page table rectangles and GPUVirtualTextureDesc metadata to gpu
	void sync_gpu_updates();

	PhysicalTilePool& get_physical_pool() noexcept { return _pool; }
	const PhysicalTilePool& get_physical_pool() const noexcept { return _pool; }

	VirtualPageTable& get_page_table() noexcept { return _pageTable; }
	const VirtualPageTable& get_page_table() const noexcept { return _pageTable; }

	VkBuffer get_vt_desc_buffer() const noexcept
	{
		return _gpuDescBuffer.is_valid() ? _gpuDescBuffer->get_buffer() : VK_NULL_HANDLE;
	}

	VkDeviceSize get_vt_desc_buffer_size() const noexcept
	{
		return _gpuDescBuffer.is_valid() ? _gpuDescBuffer->get_size() : 0;
	}

	VirtualTextureMetrics get_metrics() const;

private:	
	struct RegisteredTextureRecord
	{
		bool active{ false };
		UUID assetUuid{};
		PageTableAllocation pageTableAlloc{};
		GPUVirtualTextureDesc gpuDesc{};
	};

	void handle_evicted_page(const VirtualPageID& evictedPage);

	VulkanCore* _core{ nullptr };
	ResourceManager* _rm{ nullptr };

	PhysicalTilePool _pool;
	VirtualPageTable _pageTable;

	ResourceHandle<Buffer> _gpuDescBuffer;
	bool _descBufferDirty{ false };

	mutable std::mutex _mutex;
	std::vector<RegisteredTextureRecord> _textures;
	std::vector<uint16_t> _freeTextureIds;
	std::unordered_map<UUID, uint16_t> _uuidToTextureId;
	uint32_t _activeTextureCount{ 0 };
};