#include "VirtualTextureSystem.h"

#include <graphics/VulkanCore.h>
#include <resources/ResourceManager.h>
#include <resources/AssetRegistry.h>
#include <resources/AssetImporter.h>
#include <resources/AssetStreamer.h>
#include <graphics/utils/StagingUploader.h>
#include <core/cvars.h>
#include <fmt/core.h>

#include "utils/vk_utils.h"

AutoCVar_Int cvVTFeedbackSubsample(
	"r.vt.feedbackSubsample",
	"Screenspace tile subsample factor for VT feedback (e.g. 8 = 1 pixel per 8x8 block)",
	8,
	CVarFlags::EditInt
);

AutoCVar_Int cvVTMaxTileUploadsPerFrame(
	"r.vt.maxTileUploadsPerFrame",
	"Max virtual texture tiles queued for streaming per frame",
	16,
	CVarFlags::EditInt
);

AutoCVar_Int cvVTDebugMode(
	"r.vt.debugMode",
	"VT Debug Mode (0 = Off, 1 = Mip Heatmap, 2 = Residency Status, 3 = Physical Atlas Grid)",
	0,
	CVarFlags::EditInt
);

AutoCVar_Float cvVTMipBias(
	"r.vt.mipBias",
	"Mip bias offset applied to Virtual Texture LOD calculation",
	0.0,
	-2.0,
	4.0,
	CVarFlags::EditFloatSlider
);

void VirtualTextureSystem::init(VulkanCore* core, ResourceManager* rm, uint32_t atlasTilesPerAxis,
	uint32_t pageTableSize)
{
	_core = core;
	_rm = rm;

	StagingUploader* uploader = _rm ? &_rm->get_staging_uploader() : nullptr;

	_pool.init(_core, uploader, atlasTilesPerAxis, static_cast<uint32_t>(VTPhysicalLayer::Count), VK_FORMAT_R8G8B8A8_UNORM);
	_pageTable.init(_core, uploader, pageTableSize, VT_MAX_MIP_LEVELS);

	// 136x136 default fallback white and normal tiles 
	init_fallback_atlas();
	constexpr size_t tileTexelCount = static_cast<size_t>(VT_PHYSICAL_TILE_SIZE) * VT_PHYSICAL_TILE_SIZE;
	_defaultWhiteTile.assign(tileTexelCount * 4u, 255u);
	_defaultNormalTile.resize(tileTexelCount * 4u);
	for (size_t i = 0; i < tileTexelCount; i++)
	{
		_defaultNormalTile[i * 4 + 0] = 128u;
		_defaultNormalTile[i * 4 + 1] = 128u;
		_defaultNormalTile[i * 4 + 2] = 255u;
		_defaultNormalTile[i * 4 + 3] = 255u;
	}

	{
		std::scoped_lock<std::mutex> lock(_mutex);
		_textures.assign(MAX_VIRTUAL_TEXTURES, RegisteredTextureRecord{});

		_freeTextureIds.clear();
		for (uint32_t i = 0; i < MAX_VIRTUAL_TEXTURES; i++)
		{
			_freeTextureIds.push_back(static_cast<TextureId>(i));
		}
		_uuidToTextureId.clear();
		_inFlightPages.clear();
		_activeTextureCount = 0;
		_descBufferDirty = true;

		_feedbackFrameReady = { false, false };
		_lastFrameRawFeedbackCount = 0;
		_lastFrameUniquePages = 0;
		_lastFrameTilesQueued = 0;
		_totalTilesStreamed = 0;
	}

	if (_rm && _core)
	{
		_gpuDescBuffer = _rm->load<Buffer>("VT_MetadataDescBuffer", sizeof(GPUVirtualTextureDesc) * MAX_VIRTUAL_TEXTURES,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

		_gpuFeedbackBuffer = _rm->load<Buffer>("VT_GPUFeedbackSSBO", VT_FEEDBACK_BUFFER_SIZE_BYTES,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VMA_MEMORY_USAGE_GPU_ONLY);

		// per frame cpu mapped readback buffers
		for (uint32_t i = 0; i < FEEDBACK_FRAME_OVERLAP; i++)
		{
			std::string bufName = fmt::format("VT_FeedbackReadback_{}", i);
			_feedbackReadbackBuffers[i] = _rm->load<Buffer>(
				bufName,
				VT_FEEDBACK_BUFFER_SIZE_BYTES,
				VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				VMA_MEMORY_USAGE_GPU_TO_CPU,
				VMA_ALLOCATION_CREATE_MAPPED_BIT
			);

			if (_feedbackReadbackBuffers[i].is_valid())
			{
				if (void* mapped = _feedbackReadbackBuffers[i]->map(); mapped)
					memset(mapped, 0, VT_FEEDBACK_BUFFER_SIZE_BYTES);
			}
		}
	}
}

void VirtualTextureSystem::cleanup()
{
	std::scoped_lock<std::mutex> lock(_mutex);

	for (auto& buf : _feedbackReadbackBuffers)
		buf = {};

	_gpuFeedbackBuffer = {};
	_gpuDescBuffer = {};
	cleanup_fallback_atlas();
	_pageTable.cleanup();
	_pool.cleanup();
	_textures.clear();
	_freeTextureIds.clear();
	_uuidToTextureId.clear();
	_inFlightPages.clear();
	_activeTextureCount = 0;
	_core = nullptr;
	_rm = nullptr;
}

uint16_t VirtualTextureSystem::register_virtual_texture(const UUID& assetUuid, uint32_t virtualWidth, uint32_t virtualHeight, uint32_t mipLevels,
	std::span<const uint8_t> tailMipAlbedoPixels,
	std::span<const uint8_t> tailMipNormalPixels,
	std::span<const uint8_t> tailMipMetalRoughPixels,
	const UUID& normalAssetUuid,
	const UUID& metalRoughUuid,
	const UUID& occlusionUuid)
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

	TextureId texId = _freeTextureIds.front();
	_freeTextureIds.pop_front();

	PageTableAllocation ptAlloc = _pageTable.allocate_virtual_range(texId, virtualWidth, virtualHeight, mipLevels);
	if (!ptAlloc.is_valid())
	{
		_freeTextureIds.push_front(texId);
		return VT_INVALID_TEXTURE_ID;
	}

	RegisteredTextureRecord& rec = _textures[texId];
	rec.active = true;
	rec.assetUuid = assetUuid;
	rec.normalAssetUuid = normalAssetUuid;
	rec.metalRoughAssetUuid = metalRoughUuid;
	rec.occlusionAssetUuid = occlusionUuid;
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

	// write 1x1 tail mip colors into global fallback atlas
	const auto albedoFallback = extract_rgba(tailMipAlbedoPixels, { 255u, 255u, 255u, 255u });
	const auto normalFallback = extract_rgba(tailMipNormalPixels, { 128u, 128u, 255u, 255u });
	const auto mrFallback = extract_rgba(tailMipMetalRoughPixels, { 255u, 255u, 255u, 255u });

	stage_fallback_texel_upload(texId, albedoFallback, normalFallback, mrFallback);

	_descBufferDirty = true;
	return texId;
}

uint16_t VirtualTextureSystem::register_virtual_texture_from_asset(const UUID& albedoAssetUuid,
	const UUID& normalAssetUuid, const UUID& metalRoughAssetUuid, const UUID& occlusionAssetUuid)
{
	if (!albedoAssetUuid.is_valid())
		return VT_INVALID_TEXTURE_ID;

	TextureId staleId = VT_INVALID_TEXTURE_ID;
	{
		std::scoped_lock<std::mutex> lock(_mutex);
		auto it = _uuidToTextureId.find(albedoAssetUuid);
		if (it != _uuidToTextureId.end())
		{
			const RegisteredTextureRecord& existing = _textures[it->second];
			if (existing.normalAssetUuid == normalAssetUuid &&
				existing.metalRoughAssetUuid == metalRoughAssetUuid &&
				existing.occlusionAssetUuid == occlusionAssetUuid)
			{
				return it->second;
			}
			staleId = it->second;
		}
	}
	if (staleId != VT_INVALID_TEXTURE_ID)
		unregister_virtual_texture(staleId);

	// helper to ensure .vtex exists and read its 1x1 tail mip
	auto load_tail_mip = [](const UUID& uuid, CookedVTexHeader& outHeader, std::vector<uint8_t>& outTailPixels) -> bool
		{
			if (!uuid.is_valid())
				return false;

			std::filesystem::path vtexPath = std::filesystem::path(".cache/textures") / (uuid.to_string() + ".vtex");
			if (const AssetMetadata* meta = AssetRegistry::get().get_metadata(uuid))
			{
				if (!meta->cookedPath.empty() && meta->cookedPath.extension() == ".vtex")
					vtexPath = meta->cookedPath;
				if (!std::filesystem::exists(vtexPath))
					AssetImporter::cook_virtual_texture(*meta);
			}

			std::vector<CookedVTexTileEntry> tileTable;
			if (!AssetImporter::read_vtex_header(vtexPath, outHeader, tileTable))
				return false;

			const uint32_t tailMip = outHeader.mipLevels - 1u;
			return AssetImporter::read_vtex_tile(vtexPath, outHeader, tileTable, tailMip, 0, 0, outTailPixels);
		};

	CookedVTexHeader albedoHeader{};
	std::vector<uint8_t> tailAlbedo;
	if (!load_tail_mip(albedoAssetUuid, albedoHeader, tailAlbedo))
	{
		fmt::println("VirtualTextureSystem: failed to read .vtex header for {}", albedoAssetUuid);
		return VT_INVALID_TEXTURE_ID;
	}

	CookedVTexHeader dummyHeader{};
	std::vector<uint8_t> tailNormal;
	load_tail_mip(normalAssetUuid, dummyHeader, tailNormal);

	std::vector<uint8_t> tailMetalRough;
	bool hasMr = load_tail_mip(metalRoughAssetUuid, dummyHeader, tailMetalRough);

	std::vector<uint8_t> tailOcclusion;
	bool hasOccl = (occlusionAssetUuid.is_valid() && occlusionAssetUuid != metalRoughAssetUuid)
		? load_tail_mip(occlusionAssetUuid, dummyHeader, tailOcclusion) : false;

	// pack ao into r channel of layer 2
	if (!hasMr && hasOccl)
	{
		tailMetalRough.assign(tailOcclusion.size(), 255u);
		for (size_t i = 0; i + 3 < tailMetalRough.size(); i += 4)
			tailMetalRough[i] = tailOcclusion[i];
	}
	else if (hasMr && !(occlusionAssetUuid.is_valid() && occlusionAssetUuid == metalRoughAssetUuid))
	{
		for (size_t i = 0; i + 3 < tailMetalRough.size(); i += 4)
			tailMetalRough[i] = (hasOccl && i < tailOcclusion.size()) ? tailOcclusion[i] : 255u;
	}

	return register_virtual_texture(
		albedoAssetUuid, albedoHeader.virtualWidth, albedoHeader.virtualHeight, albedoHeader.mipLevels,
		tailAlbedo, tailNormal, tailMetalRough,
		normalAssetUuid, metalRoughAssetUuid, occlusionAssetUuid);
}

void VirtualTextureSystem::unregister_virtual_texture(TextureId textureId)
{
	std::scoped_lock<std::mutex> lock(_mutex);

	if (textureId >= _textures.size() || !_textures[textureId].active)
		return;

	if (_rm)
	{
		std::vector<VirtualPageID> cancelled;
		_rm->get_asset_streamer().cancel_vt_tiles_for_texture(textureId, cancelled);
	}

	RegisteredTextureRecord& rec = _textures[textureId];

	std::vector<VirtualPageID> freedPages;
	_pool.free_slots_for_texture(textureId, freedPages);
	_pageTable.free_virtual_range(rec.pageTableAlloc);

	std::erase_if(_inFlightPages, [textureId](const VirtualPageID& p)
		{
			return p.textureId == textureId;
		});

	if (rec.assetUuid.is_valid())
		_uuidToTextureId.erase(rec.assetUuid);

	rec = RegisteredTextureRecord{};
	_freeTextureIds.push_back(textureId);
	if (_activeTextureCount > 0)
		_activeTextureCount--;

	_descBufferDirty = true;
}

bool VirtualTextureSystem::unregister_virtual_texture_by_uuid(const UUID& assetUuid)
{
	if (!assetUuid.is_valid())
		return false;

	TextureId texId = VT_INVALID_TEXTURE_ID;
	{
		std::scoped_lock<std::mutex> lock(_mutex);
		auto it = _uuidToTextureId.find(assetUuid);
		if (it == _uuidToTextureId.end())
			return false;
		texId = it->second;
	}
	unregister_virtual_texture(texId);
	return true;
}

void VirtualTextureSystem::unregister_virtual_textures_referencing_uuid(const UUID& texUuid)
{
	if (!texUuid.is_valid())
		return;

	std::vector<TextureId> toUnregister;
	{
		std::scoped_lock<std::mutex> lock(_mutex);
		for (size_t i = 0; i < _textures.size(); i++)
		{
			const RegisteredTextureRecord& rec = _textures[i];
			if (!rec.active)
				continue;

			if (rec.assetUuid == texUuid || rec.normalAssetUuid == texUuid || rec.metalRoughAssetUuid == texUuid || rec.occlusionAssetUuid == texUuid)
				toUnregister.push_back(static_cast<TextureId>(i));
		}
	}
	for (TextureId id : toUnregister)
		unregister_virtual_texture(id);
}

bool VirtualTextureSystem::is_uuid_registered(const UUID& assetUuid) const noexcept
{
	if (!assetUuid.is_valid())
		return false;

	std::scoped_lock<std::mutex> lock(_mutex);
	return _uuidToTextureId.contains(assetUuid);
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
		std::span<const uint8_t> albedoSpan = albedoPixels.empty() ? std::span<const uint8_t>(_defaultWhiteTile) : albedoPixels;
		std::span<const uint8_t> normalSpan = normalPixels.empty() ? std::span<const uint8_t>(_defaultNormalTile) : normalPixels;
		std::span<const uint8_t> mrSpan = metalRoughPixels.empty() ? std::span<const uint8_t>(_defaultWhiteTile) : metalRoughPixels;

		_pool.stage_tile_upload(slot.slotIndex, static_cast<uint32_t>(VTPhysicalLayer::Albedo), albedoSpan);
		_pool.stage_tile_upload(slot.slotIndex, static_cast<uint32_t>(VTPhysicalLayer::Normal), normalSpan);
		_pool.stage_tile_upload(slot.slotIndex, static_cast<uint32_t>(VTPhysicalLayer::MetalRoughOccl), mrSpan);
	}

	_pageTable.map_page(rec.pageTableAlloc, pageId.tileX, pageId.tileY, pageId.mipLevel, slot.tileX, slot.tileY);
	return true;
}

void VirtualTextureSystem::process_feedback_readback(uint32_t frameSlot, uint64_t currentFrame)
{
	const uint32_t slot = frameSlot % FEEDBACK_FRAME_OVERLAP;
	ResourceHandle<Buffer>& readbackBuf = _feedbackReadbackBuffers[slot];
	if (!readbackBuf.is_valid())
		return;

	const void* mappedPtr = readbackBuf->map();
	if (!mappedPtr)
		return;

	const auto* header = static_cast<const GPUVTFeedbackHeader*>(mappedPtr);
	const uint32_t rawCount = std::min(header->requestCount, VT_MAX_FEEDBACK_REQUESTS);
	if (rawCount == 0)
	{
		std::scoped_lock<std::mutex> lock(_mutex);
		_lastFrameRawFeedbackCount = 0;
		_lastFrameUniquePages = 0;
		_lastFrameTilesQueued = 0;
		return;
	}

	const auto* requestPtr = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mappedPtr) + sizeof(GPUVTFeedbackHeader));
	std::vector<VTTileStreamRequest> queuedRequests;
	process_raw_feedback_request(std::span<const uint32_t>(requestPtr, rawCount), currentFrame, queuedRequests);

	// clear cpu readback header count so we never reprocess stale data if a pass is disabled
	const_cast<GPUVTFeedbackHeader*>(header)->requestCount = 0;

	// dispatch prioritized missing tile requests to AssetStreamer worker threads
	if (_rm && !queuedRequests.empty())
	{
		AssetStreamer& streamer = _rm->get_asset_streamer();
		for (const VTTileStreamRequest& req : queuedRequests)
			streamer.request_vt_tile(req);
	}

	{
		std::scoped_lock<std::mutex> lock(_mutex);
		_currentFrame = currentFrame;
	}
}

void VirtualTextureSystem::process_raw_feedback_request(std::span<const uint32_t> rawRequests, uint64_t currentFrame,
	std::vector<VTTileStreamRequest>& outQueuedRequests)
{
	outQueuedRequests.clear();
	if (rawRequests.empty())
		return;

	// copt and sort non zero packed requests
	std::vector<uint32_t> sortedRaw;
	sortedRaw.reserve(rawRequests.size());
	for (uint32_t r : rawRequests)
	{
		if (r != 0)
			sortedRaw.push_back(r);
	}

	if (sortedRaw.empty())
		return;

	std::sort(sortedRaw.begin(), sortedRaw.end());

	struct DedupedRequest
	{
		VirtualPageID pageId;
		uint32_t hitCount;
	};
	std::vector<DedupedRequest> uniqueRequests;
	uniqueRequests.reserve(256);

	uint32_t currentVal = sortedRaw[0];
	uint32_t currentCount = 1;
	for (size_t i = 1; i < sortedRaw.size(); i++)
	{
		if (sortedRaw[i] == currentVal)
			currentCount++;
		else
		{
			uniqueRequests.push_back({ VTFeedbackRequest::unpack(currentVal), currentCount });
			currentVal = sortedRaw[i];
			currentCount = 1;
		}
	}
	uniqueRequests.push_back({ VTFeedbackRequest::unpack(currentVal), currentCount });

	std::scoped_lock<std::mutex> lock(_mutex);
	_lastFrameRawFeedbackCount = static_cast<uint32_t>(sortedRaw.size());
	_lastFrameUniquePages = static_cast<uint32_t>(uniqueRequests.size());

	// map of missing candidate pages to accumulated hit count 
	std::unordered_map<VirtualPageID, uint32_t> candidatePageHits;
	for (const DedupedRequest& item : uniqueRequests)
	{
		const VirtualPageID& reqPage = item.pageId;
		if (reqPage.textureId >= _textures.size() || !_textures[reqPage.textureId].active)
			continue;

		const RegisteredTextureRecord& texRec = _textures[reqPage.textureId];
		const PageTableAllocation& ptAlloc = texRec.pageTableAlloc;
		if (reqPage.mipLevel >= ptAlloc.mipLevels)
			continue;

		// walk from requested mipLevel up to coarsest mip to ensure parent fallbacks are resident or queued before finer child mips
		for (uint8_t mip = reqPage.mipLevel; mip < ptAlloc.mipLevels; mip++)
		{
			const uint8_t shift = mip - reqPage.mipLevel;
			const uint16_t mipTileX = reqPage.tileX >> shift;
			const uint16_t mipTileY = reqPage.tileY >> shift;
			const uint32_t mipTilesX = std::max(1u, ptAlloc.tilesX >> mip);
			const uint32_t mipTilesY = std::max(1u, ptAlloc.tilesY >> mip);

			if (mipTileX >= mipTilesX || mipTileY >= mipTilesY)
				break;

			VirtualPageID chainPage{ reqPage.textureId, mipTileX, mipTileY, mip };

			// check if htis page is already resident in pool
			if (auto residentSlot = _pool.find_resident_page(chainPage); residentSlot.has_value())
			{
				// touch slot in lru so visible tiles are not evicted
				_pool.touch_slot(residentSlot->slotIndex, currentFrame);
				if (mip > reqPage.mipLevel)
					break;
			}
			else if (!_inFlightPages.contains(chainPage))
				candidatePageHits[chainPage] += item.hitCount;
		}
	}

	// build prioritized list of missing pages
	std::vector<VTTileStreamRequest> candidates;
	candidates.reserve(candidatePageHits.size());

	for (const auto& [pageId, hitCount] : candidatePageHits)
	{
		const RegisteredTextureRecord& texRec = _textures[pageId.textureId];

		// higher mip levels get hirher prio so parent fallbacks always load first
		// within same mip level, pages covering more screen pixels load first
		const uint32_t priorityScore = (static_cast<uint32_t>(pageId.mipLevel) * 10000u) + std::min(hitCount, 9999u);

		// queue VTTileStreamRequest 
		candidates.emplace_back(pageId, texRec.assetUuid, texRec.normalAssetUuid, texRec.metalRoughAssetUuid, texRec.occlusionAssetUuid, priorityScore);
	}

	std::sort(candidates.begin(), candidates.end(), [](const VTTileStreamRequest& a, const VTTileStreamRequest& b)
		{
			if (a.priorityScore != b.priorityScore)
			{
				return a.priorityScore > b.priorityScore;
			}
			return a.pageId.to_u64() < b.pageId.to_u64();
		});

	const uint32_t maxUploads = static_cast<uint32_t>(std::max(1, cvVTMaxTileUploadsPerFrame.Get()));
	const uint32_t countToQueue = std::min(static_cast<uint32_t>(candidates.size()), maxUploads);

	outQueuedRequests.reserve(countToQueue);
	for (uint32_t i = 0; i < countToQueue; i++)
	{
		_inFlightPages.insert(candidates[i].pageId);
		outQueuedRequests.push_back(candidates[i]);
	}
	_lastFrameTilesQueued = countToQueue;
}

void VirtualTextureSystem::notify_tile_stream_complete(const VirtualPageID& pageId, uint64_t currentFrame,
	std::span<const uint8_t> albedoPixels, std::span<const uint8_t> normalPixels,
	std::span<const uint8_t> metalRoughPixels, bool success)
{
	uint64_t resolvedFrame = currentFrame;
	if (resolvedFrame == 0)
	{
		std::scoped_lock<std::mutex> lock(_mutex);
		resolvedFrame = _currentFrame;
	}

	if (success)
		map_streamed_tile(pageId, resolvedFrame, false, albedoPixels, normalPixels, metalRoughPixels);

	std::scoped_lock<std::mutex> lock(_mutex);
	_inFlightPages.erase(pageId);
	if (success)
		_totalTilesStreamed++;
}

void VirtualTextureSystem::update()
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
	m.fallbackAtlasVramBytes = _fallbackVramBytes;

	m.lastFrameRawFeedbackCount = _lastFrameRawFeedbackCount;
	m.lastFrameUniquePagesRequested = _lastFrameUniquePages;
	m.lastFrameTilesQueued = _lastFrameTilesQueued;
	m.inFlightTileRequests = static_cast<uint32_t>(_inFlightPages.size());
	m.totalTilesStreamed = _totalTilesStreamed;
	return m;
}

void VirtualTextureSystem::init_fallback_atlas()
{
	if (!_core)
		return;

	constexpr uint32_t layerCount = static_cast<uint32_t>(VTPhysicalLayer::Count);

	VkImageCreateInfo imgInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { VT_FALLBACK_ATLAS_DIM, VT_FALLBACK_ATLAS_DIM, 1},
		.mipLevels = 1,
		.arrayLayers = layerCount,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
	};

	VmaAllocationCreateInfo allocInfo{
		.usage = VMA_MEMORY_USAGE_GPU_ONLY,
		.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
	};

	VmaAllocationInfo vmaInfo{};
	VK_CHECK(vmaCreateImage(_core->get_allocator(), &imgInfo, &allocInfo, &_fallbackAtlasImage, &_fallbackAtlasAlloc, &vmaInfo));
	_fallbackVramBytes = vmaInfo.size;

	VkImageViewCreateInfo viewInfo{};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = _fallbackAtlasImage;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.baseMipLevel = 0;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.baseArrayLayer = 0;
	viewInfo.subresourceRange.layerCount = layerCount;
	VK_CHECK(vkCreateImageView(_core->get_device(), &viewInfo, nullptr, &_fallbackAtlasView));

	_fallbackLayerViews.resize(layerCount, VK_NULL_HANDLE);
	for (uint32_t layer = 0; layer < layerCount; layer++)
	{
		VkImageViewCreateInfo layerViewInfo = viewInfo;
		layerViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		layerViewInfo.subresourceRange.baseArrayLayer = layer;
		layerViewInfo.subresourceRange.layerCount = 1;
		VK_CHECK(vkCreateImageView(_core->get_device(), &layerViewInfo, nullptr, &_fallbackLayerViews[layer]));
	}

	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_NEAREST;
	samplerInfo.minFilter = VK_FILTER_NEAREST;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.minLod = 0.0f;
	samplerInfo.maxLod = 0.0f;
	VK_CHECK(vkCreateSampler(_core->get_device(), &samplerInfo, nullptr, &_fallbackSampler));

	// clear layers to neutral pbr defaults 
	_core->immediate_submit([this, layerCount](VkCommandBuffer cmd)
		{
			// transition to transfer dst layout to clear
			vkutil::image_mem_barrier2(cmd, _fallbackAtlasImage, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layerCount, 1, 0);

			// layer 0 albedo, layer 2 metalrough: white (1, 1, 1, 1)
			VkClearColorValue whiteClear{ {1.0f, 1.0f, 1.0f, 1.0f} };
			VkImageSubresourceRange range0{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			VkImageSubresourceRange range2{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 2, 1 };
			vkCmdClearColorImage(cmd, _fallbackAtlasImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &whiteClear, 1, &range0);
			vkCmdClearColorImage(cmd, _fallbackAtlasImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &whiteClear, 1, &range2);

			// layer 1 normal: flat normal (0.5, 0.5, 1.0, 1.0)
			VkClearColorValue normalClear{ {0.5f, 0.5f, 1.0f, 1.0f} };
			VkImageSubresourceRange range1{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, 1 };
			vkCmdClearColorImage(cmd, _fallbackAtlasImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &normalClear, 1, &range1);

			// transition to shader read only
			vkutil::image_mem_barrier2(cmd, _fallbackAtlasImage, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, layerCount, 1, 0);
		});
}

void VirtualTextureSystem::cleanup_fallback_atlas()
{
	if (_core)
	{
		VkDevice dev = _core->get_device();
		if (_fallbackSampler != VK_NULL_HANDLE)
		{
			vkDestroySampler(dev, _fallbackSampler, nullptr);
			_fallbackSampler = VK_NULL_HANDLE;
		}
		for (VkImageView view : _fallbackLayerViews)
		{
			if (view != VK_NULL_HANDLE)
				vkDestroyImageView(dev, view, nullptr);
		}
		_fallbackLayerViews.clear();

		if (_fallbackAtlasView != VK_NULL_HANDLE)
		{
			vkDestroyImageView(dev, _fallbackAtlasView, nullptr);
			_fallbackAtlasView = VK_NULL_HANDLE;
		}
		if (_fallbackAtlasImage != VK_NULL_HANDLE)
		{
			vmaDestroyImage(_core->get_allocator(), _fallbackAtlasImage, _fallbackAtlasAlloc);
			_fallbackAtlasImage = VK_NULL_HANDLE;
			_fallbackAtlasAlloc = VK_NULL_HANDLE;
		}
	}
	_fallbackVramBytes = 0;
}

void VirtualTextureSystem::stage_fallback_texel_upload(TextureId textureId, const std::array<uint8_t, 4>& albedoRgba,
	const std::array<uint8_t, 4>& normalRgba, const std::array<uint8_t, 4>& metalRoughRgba)
{
	if (!_rm || _fallbackAtlasImage == VK_NULL_HANDLE || textureId >= MAX_VIRTUAL_TEXTURES)
		return;

	StagingUploader& uploader = _rm->get_staging_uploader();
	const int32_t coordX = static_cast<int32_t>(textureId % VT_FALLBACK_ATLAS_DIM);
	const int32_t coordY = static_cast<int32_t>(textureId / VT_FALLBACK_ATLAS_DIM);
	constexpr uint32_t totalLayers = static_cast<uint32_t>(VTPhysicalLayer::Count);

	const std::array<const uint8_t*, 3> layerPixels = { albedoRgba.data(), normalRgba.data(), metalRoughRgba.data() };
	for (uint32_t layer = 0; layer < totalLayers; layer++)
	{
		StagedTextureRegionDesc desc{
			.dstImage = _fallbackAtlasImage,
			.format = VK_FORMAT_R8G8B8A8_UNORM,
			.offset = { coordX, coordY, 0},
			.extent = { 1, 1, 1},
			.mipLevel = 0,
			.totalMipLevels = 1,
			.baseArrayLayer = layer,
			.layerCount = 1,
			.totalArrayLayers = totalLayers,
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		};

		uploader.stage_texture_region_upload(desc, layerPixels[layer], 4u);
	}
}

std::array<uint8_t, 4> VirtualTextureSystem::extract_rgba(std::span<const uint8_t> pixelData,
                                                          std::array<uint8_t, 4> defaultColor) noexcept
{
	if (pixelData.size() < 4)
		return defaultColor;

	// if caller passed 1x1 texel directly
	if (pixelData.size() == 4)
	{
		return { pixelData[0], pixelData[1], pixelData[2], pixelData[3] };
	}
	
	// if caller passed 136x136 physical tile from .vtex, average 128x128 logical texels
	constexpr size_t fullTileBytes = static_cast<size_t>(VT_PHYSICAL_TILE_SIZE) * VT_PHYSICAL_TILE_SIZE * 4u;
	if (pixelData.size() >= fullTileBytes)
	{
		uint64_t rSum = 0, gSum = 0, bSum = 0, aSum = 0;
		for (uint32_t y = VT_TILE_BORDER; y < VT_TILE_BORDER + VT_LOGICAL_TILE_SIZE; y++)
		{
			for (uint32_t x = VT_TILE_BORDER; x < VT_TILE_BORDER + VT_LOGICAL_TILE_SIZE; x++)
			{
				const size_t idx = (static_cast<size_t>(y) * VT_PHYSICAL_TILE_SIZE + x) * 4u;
				rSum += pixelData[idx + 0];
				gSum += pixelData[idx + 1];
				bSum += pixelData[idx + 2];
				aSum += pixelData[idx + 3];
			}
		}
		constexpr uint64_t count = static_cast<uint64_t>(VT_LOGICAL_TILE_SIZE) * VT_LOGICAL_TILE_SIZE;
		return {
			static_cast<uint8_t>((rSum + count / 2) / count),
			static_cast<uint8_t>((gSum + count / 2) / count),
			static_cast<uint8_t>((bSum + count / 2) / count),
			static_cast<uint8_t>((aSum + count / 2) / count)
		};
	}
	
	return { pixelData[0], pixelData[1], pixelData[2], pixelData[3] };
}

void VirtualTextureSystem::handle_evicted_page(const VirtualPageID& evictedPage)
{
	if (evictedPage.textureId < _textures.size() && _textures[evictedPage.textureId].active)
	{
		const PageTableAllocation& alloc = _textures[evictedPage.textureId].pageTableAlloc;
		_pageTable.unmap_page(alloc, evictedPage.tileX, evictedPage.tileY, evictedPage.mipLevel);
	}
}
