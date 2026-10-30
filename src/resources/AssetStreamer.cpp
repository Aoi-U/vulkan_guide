#include "AssetStreamer.h"

#include <vt/VirtualTextureSystem.h>
#include <resources/AssetRegistry.h>
#include <resources/AssetImporter.h>
#include <resources/ResourceManager.h>
#include <resources/Mesh.h>
#include <resources/Texture.h>
#include <fmt/core.h>
#include <fstream>
#include <algorithm>


namespace
{
	// thread local pool of open std::ifstream handles for .vtex files
	class VTexStreamPool
	{
	public:
		static constexpr size_t MAX_OPEN_STREAMS_PER_WORKER = 16;

		std::ifstream* get_stream(const UUID& uuid, const std::filesystem::path& vtexPath)
		{
			auto it = _streams.find(uuid);
			if (it != _streams.end() && it->second.is_open())
			{
				it->second.clear();
				return &it->second;
			}

			if (_streams.size() >= MAX_OPEN_STREAMS_PER_WORKER)
				_streams.erase(_streams.begin());

			std::ifstream stream(vtexPath, std::ios::binary);
			if (!stream.is_open())
				return nullptr;

			auto [insertIt, _] = _streams.emplace(uuid, std::move(stream));
			return &insertIt->second;
		}

		void clear()
		{
			_streams.clear();
		}

	private:
		std::unordered_map<UUID, std::ifstream> _streams;
	};

	thread_local VTexStreamPool tls_vtexStreams;
}

void AssetStreamer::init(ResourceManager* rm, uint32_t workerCount)
{
	_rm = rm;
	_shutdown.store(false, std::memory_order_release);

	uint32_t count = std::max(1u, workerCount);
	_workers.reserve(count);
	for (uint32_t i = 0; i < count; i++) {
		_workers.emplace_back(&AssetStreamer::worker_loop, this);
	}
}

void AssetStreamer::cleanup()
{
	_shutdown.store(true, std::memory_order_release);
	_cv.notify_all();

	for (auto& worker : _workers) {
		if (worker.joinable()) {
			worker.join();
		}
	}
	_workers.clear();
	_requestQueue.clear();
	_inFlightRequests.clear();
	_vtTileHeap.clear();
	_inFlightTiles.clear();

	std::scoped_lock<std::mutex> cacheLock(_vtexCacheMutex);
	_vtexHeaderCache.clear();
}

void AssetStreamer::request_load(const UUID& uuid)
{
	if (!uuid.is_valid())
		return;

	{
		std::scoped_lock<std::mutex> lock(_queueMutex);
		if (_inFlightRequests.contains(uuid)) {
			return;
		}

		// mark in flight and add to queue
		_inFlightRequests.insert(uuid);
		_requestQueue.push_back(uuid);
		AssetRegistry::get().set_resource_state(uuid, ResourceState::Queued);
	}
	_cv.notify_one(); // notify a worker thread that a new request is available
}

void AssetStreamer::request_vt_tile(const VTTileStreamRequest& request)
{
	if (!request.pageId.is_valid() || !request.albedoAssetUuid.is_valid())
		return;

	std::optional<VirtualPageID> droppedPage{};

	{
		std::scoped_lock<std::mutex> lock(_queueMutex);

		if (_inFlightTiles.contains(request.pageId))
		{
			for (auto& existing : _vtTileHeap)
			{
				if (existing.pageId == request.pageId && request.priorityScore > existing.priorityScore)
				{
					existing.priorityScore = request.priorityScore;
					std::ranges::make_heap(_vtTileHeap, VTTileHeapCompare{});
					break;
				}
			}
			return;
		}

		// if pending heap is max capacity, evict lowest priority queue request
		if (_vtTileHeap.size() >= VT_MAX_PENDING_REQUESTS)
		{
			auto minIt = std::ranges::min_element(_vtTileHeap, VTTileHeapCompare{});
			if (minIt != _vtTileHeap.end())
			{
				// incoming request is lower prio than everything in full queue
				if (request.priorityScore <= minIt->priorityScore)
					droppedPage = request.pageId;
				else
				{
					droppedPage = minIt->pageId;
					_inFlightTiles.erase(minIt->pageId);
					*minIt = _vtTileHeap.back();
					_vtTileHeap.pop_back();
					std::ranges::make_heap(_vtTileHeap, VTTileHeapCompare{});
				}
			}
		}

		if (!droppedPage.has_value() || *droppedPage != request.pageId)
		{
			_inFlightTiles.insert(request.pageId);
			_vtTileHeap.push_back(request);
			std::ranges::push_heap(_vtTileHeap, VTTileHeapCompare{});
		}
	}

	if (droppedPage.has_value() && _rm)
		_rm->get_vtsystem().notify_tile_stream_complete(*droppedPage, 0, {}, {}, {}, false);
	_cv.notify_one();
}

void AssetStreamer::cancel_vt_tiles_for_texture(uint16_t textureId, std::vector<VirtualPageID>& outCancelledPages)
{
	std::scoped_lock<std::mutex> lock(_queueMutex);
	outCancelledPages.clear();

	auto removedIt = std::remove_if(_vtTileHeap.begin(), _vtTileHeap.end(), [&](const VTTileStreamRequest& req)
		{
			if (req.pageId.textureId == textureId)
			{
				outCancelledPages.push_back(req.pageId);
				_inFlightTiles.erase(req.pageId);
				return true;
			}

			return false;
		});

	if (removedIt != _vtTileHeap.end())
	{
		_vtTileHeap.erase(removedIt, _vtTileHeap.end());
		std::ranges::make_heap(_vtTileHeap, VTTileHeapCompare{});
	}
}

void AssetStreamer::invalidate_vtex_cache(const UUID& texUuid)
{
	std::scoped_lock<std::mutex> lock(_vtexCacheMutex);
	_vtexHeaderCache.erase(texUuid);
}

size_t AssetStreamer::get_pending_vt_tile_count() const
{
	std::scoped_lock<std::mutex> lock(_queueMutex);
	return _vtTileHeap.size();
}

void AssetStreamer::worker_loop()
{
	while (!_shutdown.load(std::memory_order_acquire)) {
		VTTileStreamRequest nextTileReq{};
		UUID nextAssetUuid{};
		bool hasTileReq = false;

		{
			std::unique_lock<std::mutex> lock(_queueMutex);
			// wait until there is a request in queue or shutdown requested
			_cv.wait(lock, [this]() {
				return _shutdown.load(std::memory_order_acquire) || !_vtTileHeap.empty() || !_requestQueue.empty();
				});

			if (_shutdown.load(std::memory_order_acquire))
			{
				tls_vtexStreams.clear();
				return;
			}

			if (!_vtTileHeap.empty())
			{
				std::ranges::pop_heap(_vtTileHeap, VTTileHeapCompare{});
				nextTileReq = _vtTileHeap.back();
				_vtTileHeap.pop_back();
				hasTileReq = true;
			}
			else
			{
				// get next request from queue
				nextAssetUuid = _requestQueue.front();
				_requestQueue.pop_front();
			}
		}

		// process asset request outside 
		// // remove from in flight requests after processing
		if (hasTileReq)
		{
			process_vt_tile_request(nextTileReq);
			{
				std::scoped_lock<std::mutex> lock(_queueMutex);
				_inFlightTiles.erase(nextTileReq.pageId);
			}
		}
		else
		{
			process_asset_request(nextAssetUuid);
			{
				std::scoped_lock<std::mutex> lock(_queueMutex);
				_inFlightRequests.erase(nextAssetUuid);
			}
		}
	}
}

void AssetStreamer::process_asset_request(const UUID& uuid)
{
	const AssetMetadata* meta = AssetRegistry::get().get_metadata(uuid);
	if (!meta) {
		AssetRegistry::get().set_resource_state(uuid, ResourceState::Failed);
		return;
	}

	AssetRegistry::get().set_resource_state(uuid, ResourceState::Loading);

	AssetType type = meta->type;
	std::string name = meta->name;
	std::filesystem::path virtualPath = meta->virtualPath;
	std::filesystem::path cookedPath = meta->cookedPath;
	TextureImportSettings texSettings = meta->textureSettings;

	// ensure cooked binary exists on disk on worker thread before queuing gpu handle creation
	if (type == AssetType::Texture && !cookedPath.empty() && !std::filesystem::exists(cookedPath) && !meta->is_sub_asset()) {
		AssetImporter::cook_texture(*meta);
	}

	if (type == AssetType::Mesh) {
		_rm->queue_main_thread_task([rm = _rm, uuid, name, cookedPath]() {
			rm->load_by_uuid<Mesh>(uuid, name, cookedPath);
			});
	}
	else if (type == AssetType::Texture) {
		if (texSettings.virtualTexture)
		{
			_rm->queue_main_thread_task([rm = _rm, uuid]()
				{
					rm->get_vtsystem().register_virtual_texture_from_asset(uuid);
					AssetRegistry::get().set_resource_state(uuid, ResourceState::Loaded);
				});
		}
		else 
		{
			VkFormat fmt = texSettings.sRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
			_rm->queue_main_thread_task([rm = _rm, uuid, name, virtualPath, fmt, texSettings]() {
				rm->load_by_uuid<Texture>(uuid, name, virtualPath, fmt, VK_IMAGE_USAGE_SAMPLED_BIT, texSettings.generateMips);
				});
		}
	}
}

void AssetStreamer::process_vt_tile_request(const VTTileStreamRequest& req)
{
	if (!_rm)
		return;

	if (!_rm->get_vtsystem().is_texture_active(req.pageId.textureId))
	{
		_rm->get_vtsystem().notify_tile_stream_complete(req.pageId, 0, {}, {}, {}, false);
		return;
	}

	std::vector<uint8_t> albedoTile;
	std::vector<uint8_t> normalTile;
	std::vector<uint8_t> metalRoughTile;
	std::vector<uint8_t> occlusionTile;

	bool ok = load_tile_from_vtex_cached(req.albedoAssetUuid, req.pageId.mipLevel, req.pageId.tileX, req.pageId.tileY, albedoTile);

	if (ok && req.normalAssetUuid.is_valid())
		load_tile_from_vtex_cached(req.normalAssetUuid, req.pageId.mipLevel, req.pageId.tileX, req.pageId.tileY, normalTile);

	bool hasMR = false;
	if (ok && req.metalRoughAssetUuid.is_valid())
		hasMR = load_tile_from_vtex_cached(req.metalRoughAssetUuid, req.pageId.mipLevel, req.pageId.tileX, req.pageId.tileY, metalRoughTile);

	bool hasOccl = false;
	if (ok && req.occlusionAssetUuid.is_valid() && req.occlusionAssetUuid != req.metalRoughAssetUuid)
		hasOccl = load_tile_from_vtex_cached(req.occlusionAssetUuid, req.pageId.mipLevel, req.pageId.tileX, req.pageId.tileY, occlusionTile);

	// pack ao into r channel of layer 2
	if (ok)
	{
		if (!hasMR && hasOccl)
		{
			metalRoughTile.assign(occlusionTile.size(), 255u);
			for (size_t i = 0; i + 3 < metalRoughTile.size(); i += 4)
				metalRoughTile[i + 0] = occlusionTile[i + 0];
		}
		else if (hasMR && !(req.occlusionAssetUuid.is_valid() && req.occlusionAssetUuid == req.metalRoughAssetUuid))
		{
			for (size_t i = 0; i + 3 < metalRoughTile.size(); i += 4)
				metalRoughTile[i + 0] = (hasOccl && i < occlusionTile.size()) ? occlusionTile[i + 0] : 255u;
		}
	}

	VirtualPageID pageId = req.pageId;
	_rm->queue_main_thread_task([rm = _rm, pageId, ok,
		albedo = std::move(albedoTile),
		normal = std::move(normalTile),
		metalRough = std::move(metalRoughTile)]() mutable
		{
			rm->get_vtsystem().notify_tile_stream_complete(pageId, 0, albedo, normal, metalRough, ok);
		});
}

bool AssetStreamer::load_tile_from_vtex_cached(const UUID& texUuid, uint8_t mipLevel, uint16_t tileX, uint16_t tileY,
	std::vector<uint8_t>& outPixels)
{
	if (!texUuid.is_valid())
		return false;

	const CachedVTexMetadata* metaPtr = nullptr;
	{
		std::scoped_lock<std::mutex> lock(_vtexCacheMutex);
		auto it = _vtexHeaderCache.find(texUuid);
		if (it != _vtexHeaderCache.end())
			metaPtr = &it->second;
	}

	if (!metaPtr)
	{
		CachedVTexMetadata loadedMeta{};
		loadedMeta.vtexPath = std::filesystem::path(".cache/textures") / (texUuid.to_string() + ".vtex");

		if (const AssetMetadata* meta = AssetRegistry::get().get_metadata(texUuid))
		{
			if (!meta->cookedPath.empty() && meta->cookedPath.extension() == ".vtex")
				loadedMeta.vtexPath = meta->cookedPath;
			if (!std::filesystem::exists(loadedMeta.vtexPath) && std::filesystem::exists(meta->virtualPath))
				AssetImporter::cook_virtual_texture(*meta);
		}

		if (!AssetImporter::read_vtex_header(loadedMeta.vtexPath, loadedMeta.header, loadedMeta.tileTable))
			return false;

		std::scoped_lock<std::mutex> lock(_vtexCacheMutex);
		auto [insertedIt, _] = _vtexHeaderCache.emplace(texUuid, std::move(loadedMeta));
		metaPtr = &insertedIt->second;
	}

	const CookedVTexHeader& header = metaPtr->header;
	if (header.mipLevels == 0 || header.mipLevels > MAX_VTEX_MIPS)
		return false;

	const uint32_t clampedMip = std::min(static_cast<uint32_t>(mipLevel), header.mipLevels - 1u);
	const CookedVTexMipInfo& mipInfo = header.mips[clampedMip];
	const uint32_t clampedTileX = std::min(static_cast<uint32_t>(tileX), mipInfo.tilesX - 1u);
	const uint32_t clampedTileY = std::min(static_cast<uint32_t>(tileY), mipInfo.tilesY - 1u);

	const uint32_t flatTileIndex = mipInfo.firstTileIndex + (clampedTileY * mipInfo.tilesX + clampedTileX);
	if (flatTileIndex >= metaPtr->tileTable.size())
		return false;

	const CookedVTexTileEntry& entry = metaPtr->tileTable[flatTileIndex];
	if (entry.byteSize == 0)
		return false;

	// reuse persistent ifstream handle
	std::ifstream* stream = tls_vtexStreams.get_stream(texUuid, metaPtr->vtexPath);
	if (!stream)
		return false;

	stream->seekg(static_cast<std::streamoff>(entry.byteOffset), std::ios::beg);
	if (!stream->good())
		return false;

	outPixels.resize(entry.byteSize);
	stream->read(reinterpret_cast<char*>(outPixels.data()), static_cast<std::streamsize>(entry.byteSize));
	return stream->good();
}
