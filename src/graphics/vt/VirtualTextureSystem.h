#pragma once

#include <graphics/vt/VirtualTextureTypes.h>
#include <graphics/vt/PhysicalTilePool.h>
#include <graphics/vt/VirtualPageTable.h>
#include <resources/Buffer.h>
#include <core/UUID.h>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <span>
#include <mutex>
#include <deque>

class VulkanCore;
class ResourceManager;

// system managing virtual textures, including physical tile pool, virtual page table, and related resources
class VirtualTextureSystem
{
public:
	using TextureId = uint16_t;
	static constexpr uint32_t MAX_VIRTUAL_TEXTURES = 4096;
	static constexpr uint32_t FEEDBACK_FRAME_OVERLAP = 2;

	void init(VulkanCore* core, ResourceManager* rm, uint32_t atlasTilesPerAxis = VT_DEFAULT_ATLAS_TILES_PER_AXIS, uint32_t pageTableSize = VT_DEFAULT_PAGE_TABLE_SIZE);

	void cleanup();

	// register a virtual texture in the page table, and pins its 1x1 tail mip in  the physical pool
	// return its runtime virtual texture id
	TextureId register_virtual_texture(const UUID& assetUuid, uint32_t virtualWidth, uint32_t virtualHeight, uint32_t mipLevels,
		std::span<const uint8_t> tailMipAlbedoPixels = {}, 
		std::span<const uint8_t> tailMipNormalPixels = {},
		std::span<const uint8_t> tailMipMetalRoughPixels = {},
		const UUID& normalAssetUuid = UUID::null(), 
		const UUID& metalRoughUuid = UUID::null(),
		const UUID& occlusionUuid = UUID::null()
		);

	// reads cooked .vtex asset header from disk
	// extract 1x1 tail mip and register with virtualtexturesystem
	TextureId register_virtual_texture_from_asset(
		const UUID& albedoAssetUuid,
		const UUID& normalAssetUuid = UUID::null(),
		const UUID& metalRoughAssetUuid = UUID::null(),
		const UUID& occlusionAssetUuid = UUID::null()
	);

	// unregisters a virtual texture
	// frees its physical tiles and page table allocation
	void unregister_virtual_texture(TextureId textureId);

	// unregister virtual texture by primary (albedo) asset uuid
	bool unregister_virtual_texture_by_uuid(const UUID& assetUuid);

	// unregister any virtual texture that references texUuid in any PBR layer
	void unregister_virtual_textures_referencing_uuid(const UUID& texUuid);

	// check if albedo tex uuid is registered in virtualtexturesystem
	bool is_uuid_registered(const UUID& assetUuid) const noexcept;

	// map streamed tile into physical pool and update virtual page table
	bool map_streamed_tile(const VirtualPageID& pageId, uint64_t currentFrame, bool pinTile,
		std::span<const uint8_t> albedoPixels,
		std::span<const uint8_t> normalPixels = {},
		std::span<const uint8_t> metalRoughPixels = {}
	);

	// process cpu mapped feedback readback buffer from completed gpu frame
	void process_feedback_readback(uint32_t frameSlot, uint64_t currentFrame);

	// directly process span of raw packed VTFeedbackRequest uint32s
	void process_raw_feedback_request(std::span<const uint32_t> rawRequests, uint64_t currentFrame, std::vector<VTTileStreamRequest>& outQueuedRequests);

	void notify_tile_stream_complete(const VirtualPageID& pageId, uint64_t currentFrame,
		std::span<const uint8_t> albedoPixels,
		std::span<const uint8_t> normalPixels = {},
		std::span<const uint8_t> metalRoughPixels = {},
		bool success = true
	);

	bool is_texture_active(TextureId textureId) const noexcept
	{
		std::scoped_lock<std::mutex> lock(_mutex);
		return textureId < _textures.size() && _textures[textureId].active;
	}

	// flush dirty page table rectangles and GPUVirtualTextureDesc metadata to gpu
	void update();

	PhysicalTilePool& get_physical_pool() noexcept { return _pool; }
	const PhysicalTilePool& get_physical_pool() const noexcept { return _pool; }

	VirtualPageTable& get_page_table() noexcept { return _pageTable; }
	const VirtualPageTable& get_page_table() const noexcept { return _pageTable; }

	// global fallback atlas getters
	VkImage get_fallback_atlas_image() const noexcept { return _fallbackAtlasImage; }
	VkImageView get_fallback_atlas_view() const noexcept { return _fallbackAtlasView; }
	VkImageView get_fallback_layer_view(uint32_t layer) const noexcept
	{
		return (layer < _fallbackLayerViews.size()) ? _fallbackLayerViews[layer] : _fallbackAtlasView;
	}
	VkSampler get_fallback_atlas_sampler() const noexcept { return _fallbackSampler; }
	VkExtent3D get_fallback_atlas_extent() const noexcept { return { VT_FALLBACK_ATLAS_DIM, VT_FALLBACK_ATLAS_DIM, 1 }; }

	VkBuffer get_vt_desc_buffer() const noexcept
	{
		return _gpuDescBuffer.is_valid() ? _gpuDescBuffer->get_buffer() : VK_NULL_HANDLE;
	}

	VkDeviceSize get_vt_desc_buffer_size() const noexcept
	{
		return _gpuDescBuffer.is_valid() ? _gpuDescBuffer->get_size() : 0;
	}

	VkDeviceSize get_gpu_feedback_buffer_size() const noexcept
	{
		return _gpuFeedbackBuffer.is_valid() ? _gpuFeedbackBuffer->get_size() : 0;
	}

	VkBuffer get_feedback_readback_buffer(uint32_t frameSlot) const noexcept
	{
		const auto& buf = _feedbackReadbackBuffers[frameSlot % FEEDBACK_FRAME_OVERLAP];
		return buf.is_valid() ? buf->get_buffer() : VK_NULL_HANDLE;
	}
	VkBuffer get_feedback_buffer() const noexcept
	{
		return _gpuFeedbackBuffer.is_valid() ? _gpuFeedbackBuffer->get_buffer() : VK_NULL_HANDLE;
	}

	VirtualTextureMetrics get_metrics() const;

private:	
	struct RegisteredTextureRecord
	{
		bool active{ false };
		UUID assetUuid{};
		UUID normalAssetUuid{};
		UUID metalRoughAssetUuid{};
		UUID occlusionAssetUuid{};
		PageTableAllocation pageTableAlloc{};
		GPUVirtualTextureDesc gpuDesc{};
	};

	void init_fallback_atlas();
	void cleanup_fallback_atlas();
	void stage_fallback_texel_upload(TextureId textureId, const std::array<uint8_t, 4>& albedoRgba, const std::array<uint8_t, 4>& normalRgba, const std::array<uint8_t, 4>& metalRoughRgba);
	static std::array<uint8_t, 4> extract_rgba(std::span<const uint8_t> pixelData, std::array<uint8_t, 4> defaultColor) noexcept;

	void handle_evicted_page(const VirtualPageID& evictedPage);

	VulkanCore* _core{ nullptr };
	ResourceManager* _rm{ nullptr };
	uint64_t _currentFrame{ 0 };

	PhysicalTilePool _pool;
	VirtualPageTable _pageTable;

	// global fallback atlas of 3 64x64 pbr layers
	VkImage _fallbackAtlasImage{ VK_NULL_HANDLE };
	VmaAllocation _fallbackAtlasAlloc{ VK_NULL_HANDLE };
	VkImageView _fallbackAtlasView{ VK_NULL_HANDLE };
	std::vector<VkImageView> _fallbackLayerViews;
	VkSampler _fallbackSampler{ VK_NULL_HANDLE };
	uint64_t _fallbackVramBytes{ 0 };

	ResourceHandle<Buffer> _gpuDescBuffer;
	ResourceHandle<Buffer> _gpuFeedbackBuffer;
	std::array<ResourceHandle<Buffer>, FEEDBACK_FRAME_OVERLAP> _feedbackReadbackBuffers;
	std::array<bool, FEEDBACK_FRAME_OVERLAP> _feedbackFrameReady{ false };
	bool _descBufferDirty{ false };

	mutable std::mutex _mutex;
	std::vector<RegisteredTextureRecord> _textures;
	std::deque<TextureId> _freeTextureIds;
	std::unordered_map<UUID, TextureId> _uuidToTextureId;
	std::unordered_set<VirtualPageID> _inFlightPages;
	uint32_t _activeTextureCount{ 0 };

	std::vector<uint8_t> _defaultWhiteTile;
	std::vector<uint8_t> _defaultNormalTile;

	// metric counters
	uint32_t _lastFrameRawFeedbackCount{ 0 };
	uint32_t _lastFrameUniquePages{ 0 };
	uint32_t _lastFrameTilesQueued{ 0 };
	uint64_t _totalTilesStreamed{ 0 };
};