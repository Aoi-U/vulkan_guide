#pragma once

#include <core/UUID.h>
#include <resources/AssetMetadata.h>
#include <resources/CookedFormats.h>
#include <graphics/vt/VirtualTextureTypes.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <unordered_set>
#include <vector>
#include <atomic>

// runs worker threads that cook and streams necessary assets

class ResourceManager;

class AssetStreamer
{
public:
	static constexpr size_t VT_MAX_PENDING_REQUESTS = 521;

	void init(ResourceManager* rm, uint32_t workerCount = 2);
	void cleanup();

	// queue an asset uuid to load
	void request_load(const UUID& uuid);

	void request_vt_tile(const VTTileStreamRequest& request);
	void cancel_vt_tiles_for_texture(uint16_t textureId, std::vector<VirtualPageID>& outCancelledPages);

	size_t get_pending_vt_tile_count() const;

private:
	struct CachedVTexMetadata
	{
		std::filesystem::path vtexPath;
		CookedVTexHeader header{};
		std::vector<CookedVTexTileEntry> tileTable;
	};

	struct VTTileHeapCompare
	{
		bool operator()(const VTTileStreamRequest& a, const VTTileStreamRequest& b) const noexcept
		{
			if (a.priorityScore != b.priorityScore)
				return a.priorityScore < b.priorityScore; // largest priorityScore at front

			return a.pageId.to_u64() > b.pageId.to_u64();
		}
	};

	void worker_loop();
	void process_asset_request(const UUID& uuid);
	void process_vt_tile_request(const VTTileStreamRequest& req);
	bool load_tile_from_vtex_cached(const UUID& texUuid, uint8_t mipLevel, uint16_t tileX, uint16_t tileY, std::vector<uint8_t>& outPixels);

	ResourceManager* _rm{ nullptr };
	std::vector<std::thread> _workers;

	// whole asset queue
	std::deque<UUID> _requestQueue;
	std::unordered_set<UUID> _inFlightRequests;

	// virtual texture tile queue
	std::vector<VTTileStreamRequest> _vtTileHeap;
	std::unordered_set<VirtualPageID> _inFlightTiles;

	mutable std::mutex _queueMutex;
	std::condition_variable _cv;

	std::mutex _vtexCacheMutex;
	std::unordered_map<UUID, CachedVTexMetadata> _vtexHeaderCache;

	std::atomic<bool> _shutdown{ false };
};