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

class VulkanCore;
class ResourceManager;

// system managing virtual textures, including physical tile pool, virtual page table, and related resources
class VirtualTextureSystem
{
public:
	static constexpr uint32_t MAX_VIRTUAL_TEXTURES = 4096;
	static constexpr uint32_t FEEDBACK_FRAME_OVERLAP = 2;

	void init(VulkanCore* core, ResourceManager* rm, uint32_t atlasTilesPerAxis = VT_DEFAULT_ATLAS_TILES_PER_AXIS, uint32_t pageTableSize = VT_DEFAULT_PAGE_TABLE_SIZE);

	void cleanup();

	// register a virtual texture in the page table, and pins its 1x1 tail mip in  the physical pool
	// return its runtime virtual texture id
	uint16_t register_virtual_texture(const UUID& assetUuid, uint32_t virtualWidth, uint32_t virtualHeight, uint32_t mipLevels, 
		std::span<const uint8_t> tailMipAlbedoPixels = {}, 
		std::span<const uint8_t> tailMipNormalPixels = {},
		std::span<const uint8_t> tailMipMetalRoughPixels = {},
		const UUID& normalAssetUuid = UUID::null(), 
		const UUID& metalRoughUuid = UUID::null(),
		const UUID& occlusionUuid = UUID::null()
		);

	// reads cooked .vtex asset header from disk
	// extract 1x1 tail mip and register with virtualtexturesystem
	uint16_t register_virtual_texture_from_asset(
		const UUID& albedoAssetUuid,
		const UUID& normalAssetUuid = UUID::null(),
		const UUID& metalRoughAssetUuid = UUID::null(),
		const UUID& occlusionAssetUuid = UUID::null()
	);

	// unregisters a virtual texture
	// frees its physical tiles and page table allocation
	void unregister_virtual_texture(uint16_t textureId);

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

	bool is_texture_active(uint16_t textureId) const noexcept
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

	void handle_evicted_page(const VirtualPageID& evictedPage);

	VulkanCore* _core{ nullptr };
	ResourceManager* _rm{ nullptr };
	uint64_t _currentFrame{ 0 };

	PhysicalTilePool _pool;
	VirtualPageTable _pageTable;

	ResourceHandle<Buffer> _gpuDescBuffer;
	ResourceHandle<Buffer> _gpuFeedbackBuffer;
	std::array<ResourceHandle<Buffer>, FEEDBACK_FRAME_OVERLAP> _feedbackReadbackBuffers;
	std::array<bool, FEEDBACK_FRAME_OVERLAP> _feedbackFrameReady{ false };
	bool _descBufferDirty{ false };

	mutable std::mutex _mutex;
	std::vector<RegisteredTextureRecord> _textures;
	std::vector<uint16_t> _freeTextureIds;
	std::unordered_map<UUID, uint16_t> _uuidToTextureId;
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