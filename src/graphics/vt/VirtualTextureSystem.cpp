#include "VirtualTextureSystem.h"

#include <graphics/VulkanCore.h>
#include <resources/ResourceManager.h>
#include <graphics/utils/StagingUploader.h>
#include <fmt/core.h>

void VirtualTextureSystem::init(VulkanCore* core, ResourceManager* rm, uint32_t atlasTilesPerAxis,
	uint32_t pageTableSize)
{
	_core = core;
	_rm = rm;

	StagingUploader* uploader = _rm ? &_rm->get_staging_uploader() : nullptr;

	_pool.init(_core, uploader, atlasTilesPerAxis, static_cast<uint32_t>(VTPhysicalLayer::Count), VK_FORMAT_R8G8B8A8_UNORM);
	_pageTable.init(_core, uploader, pageTableSize, VT_MAX_MIP_LEVELS);

	{
		std::scoped_lock<std::mutex> lock(_mutex);
		_textures.assign(MAX_VIRTUAL_TEXTURES, RegisteredTextureRecord{});
		_freeTextureIds.resize(MAX_VIRTUAL_TEXTURES);
		
		for (uint32_t i = 0; i < MAX_VIRTUAL_TEXTURES; i++)
		{
			_freeTextureIds[MAX_VIRTUAL_TEXTURES - 1 - i] = static_cast<uint16_t>(i);
		}
		_uuidToTextureId.clear();
		_activeTextureCount = 0;
		_descBufferDirty = true;
	}

	if (_rm && _core)
	{
		_gpuDescBuffer = _rm->load<Buffer>("VT_MetadataDescBuffer", sizeof(GPUVirtualTextureDesc) * MAX_VIRTUAL_TEXTURES,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	}
}

void VirtualTextureSystem::cleanup()
{
	std::scoped_lock<std::mutex> lock(_mutex);
	_gpuDescBuffer = {};
	_pageTable.cleanup();
	_pool.cleanup();
	_textures.clear();
	_freeTextureIds.clear();
	_uuidToTextureId.clear();
	_activeTextureCount = 0;
	_core = nullptr;
	_rm = nullptr;
}

uint16_t VirtualTextureSystem::register_virtual_texture(const UUID& assetUuid, uint32_t virtualWidth,
	uint32_t virtualHeight, uint32_t mipLevels, std::span<const uint8_t> tailMipAlbedoPixels)
{
	std::scoped_lock<std::mutex> lock(_mutex);

	if (assetUuid.is_valid())
	{
		// check if texture is already registered
		auto it = _uuidToTextureId.find(assetUuid);
		if (it != _uuidToTextureId.end())
			return it->second;
	}

	if (_freeTextureIds.empty())
	{
		fmt::println("VirtualTextureSystem: exceeded MAX_VIRTUAL_TEXTURES {}", MAX_VIRTUAL_TEXTURES);
		return VT_INVALID_TEXTURE_ID;
	}

	uint16_t texId = _freeTextureIds.back();
	_freeTextureIds.pop_back();

	PageTableAllocation ptAlloc = _pageTable.allocate_virtual_range(texId, virtualWidth, virtualHeight, mipLevels);
	if (!ptAlloc.is_valid())
	{
		_freeTextureIds.push_back(texId);
		return VT_INVALID_TEXTURE_ID;
	}

	RegisteredTextureRecord& rec = _textures[texId];
	rec.active = true;
	rec.assetUuid = assetUuid;
	rec.pageTableAlloc = ptAlloc;
	rec.gpuDesc = GPUVirtualTextureDesc{
		.pageTableOffsetX = ptAlloc.offsetX,
		.pageTableOffsetY = ptAlloc.offsetY,
		.virtualWidth = virtualWidth,
		.virtualHeight = virtualHeight,
		.tilesX = ptAlloc.tilesX,
		.tilesY = ptAlloc.tilesY,
		.maxMipLevel = ptAlloc.mipLevels - 1u,
		.fallbackPageEntry = 0
	};

	if (assetUuid.is_valid())
		_uuidToTextureId[assetUuid] = texId;

	_activeTextureCount++;

	// pin tail mip so shaders always has valid fallback
	uint8_t tailMip = static_cast<uint8_t>(ptAlloc.mipLevels - 1u);
	VirtualPageID tailPageId{ texId, 0, 0, tailMip };
	PhysicalSlotCoord tailSlot{};
	std::optional<VirtualPageID> evicted{};
	bool wasHit = false;

	if (_pool.request_or_allocate_slot(tailPageId, 0, true, tailSlot, evicted, wasHit))
	{
		if (evicted.has_value())
			handle_evicted_page(*evicted);

		if (!tailMipAlbedoPixels.empty())
		{
			_pool.stage_tile_upload(tailSlot.slotIndex, static_cast<uint32_t>(VTPhysicalLayer::Albedo), tailMipAlbedoPixels);
		}

		_pageTable.map_page(ptAlloc, 0, 0, tailMip, tailSlot.tileX, tailSlot.tileY);
		rec.gpuDesc.fallbackPageEntry = PageTableEntry::pack(tailSlot.tileX, tailSlot.tileY, tailMip, false).raw;
	}

	_descBufferDirty = true;
	return texId;
}

void VirtualTextureSystem::unregister_virtual_texture(uint16_t textureId)
{
	std::scoped_lock<std::mutex> lock(_mutex);
	if (textureId >= _textures.size() || !_textures[textureId].active)
		return;

	RegisteredTextureRecord& rec = _textures[textureId];

	std::vector<VirtualPageID> freedPages;
	_pool.free_slots_for_texture(textureId, freedPages);
	_pageTable.free_virtual_range(rec.pageTableAlloc);

	if (rec.assetUuid.is_valid())
		_uuidToTextureId.erase(rec.assetUuid);

	rec = RegisteredTextureRecord{};
	_freeTextureIds.push_back(textureId);
	if (_activeTextureCount > 0)
		_activeTextureCount--;

	_descBufferDirty = true;
}

bool VirtualTextureSystem::map_streamed_tile(const VirtualPageID& pageId, uint64_t currentFrame, bool pinTile,
	std::span<const uint8_t> albedoPixels, std::span<const uint8_t> normalPixels,
	std::span<const uint8_t> metalRoughPixels)
{
	std::scoped_lock<std::mutex> lock(_mutex);
	
	if (pageId.textureId >= _textures.size() || !_textures[pageId.textureId].active)
		return false;

	const RegisteredTextureRecord& rec = _textures[pageId.textureId];

	PhysicalSlotCoord slot{};
	std::optional<VirtualPageID> evicted{};
	bool wasHit = false;

	if (!_pool.request_or_allocate_slot(pageId, currentFrame, pinTile, slot, evicted, wasHit))
		return false;

	// if another textures old tile was evicted to make room, unmap it in page table first
	if (evicted.has_value())
		handle_evicted_page(*evicted);

	if (!wasHit)
	{
		if (!albedoPixels.empty())
			_pool.stage_tile_upload(slot.slotIndex, static_cast<uint32_t>(VTPhysicalLayer::Albedo), albedoPixels);

		if (!normalPixels.empty())
			_pool.stage_tile_upload(slot.slotIndex, static_cast<uint32_t>(VTPhysicalLayer::Normal), normalPixels);

		if (!metalRoughPixels.empty())
			_pool.stage_tile_upload(slot.slotIndex, static_cast<uint32_t>(VTPhysicalLayer::MetalRoughOccl), metalRoughPixels);
	}

	_pageTable.map_page(rec.pageTableAlloc, pageId.tileX, pageId.tileY, pageId.mipLevel, slot.tileX, slot.tileY);
	return true;
}

void VirtualTextureSystem::sync_gpu_updates()
{
	std::scoped_lock<std::mutex> lock(_mutex);

	// flush any dirty page table rectangles to gpu texture
	_pageTable.flush_dirty_pages();

	// upload GPUVirtualTextureDesc metadata array if modified
	if (_descBufferDirty && _rm && _gpuDescBuffer.is_valid())
	{
		std::vector<GPUVirtualTextureDesc> packedDescs(_textures.size());
		for (size_t i = 0; i < _textures.size(); i++)
		{
			packedDescs[i] = _textures[i].gpuDesc;
		}

		_rm->get_staging_uploader().stage_buffer_upload(_gpuDescBuffer->get_buffer(), 0, packedDescs.data(), packedDescs.size() * sizeof(GPUVirtualTextureDesc));

		_descBufferDirty = false;
	}
}

VirtualTextureMetrics VirtualTextureSystem::get_metrics() const
{
	std::scoped_lock<std::mutex> lock(_mutex);

	VirtualTextureMetrics m{};
	m.totalPhysicalSlots = _pool.get_total_slots();
	m.usedPhysicalSlots = _pool.get_used_slots();
	m.pinnedPhysicalSlots = _pool.get_pinned_slots();
	m.registeredVirtualTextures = _activeTextureCount;
	m.allocatedPageTableTilesMip0 = _pageTable.get_allocated_tiles_mip0();
	m.totalPageTableCapacityMip0 = _pageTable.get_size_mip0() * _pageTable.get_size_mip0();
	m.totalCacheHits = _pool.get_cache_hits();
	m.totalCacheMisses = _pool.get_cache_misses();
	m.totalEvictions = _pool.get_evictions();
	m.physicalAtlasVramBytes = _pool.get_vram_bytes();
	m.pageTableVramBytes = _pageTable.get_vram_bytes();
	return m;
}

void VirtualTextureSystem::handle_evicted_page(const VirtualPageID& evictedPage)
{
	if (evictedPage.textureId < _textures.size() && _textures[evictedPage.textureId].active)
	{
		const PageTableAllocation& alloc = _textures[evictedPage.textureId].pageTableAlloc;
		_pageTable.unmap_page(alloc, evictedPage.tileX, evictedPage.tileY, evictedPage.mipLevel);
	}
}
