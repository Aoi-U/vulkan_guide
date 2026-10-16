#include "AssetStreamer.h"

#include <resources/AssetRegistry.h>
#include <resources/AssetImporter.h>
#include <resources/ResourceManager.h>
#include <resources/Mesh.h>
#include <resources/Texture.h>
#include <fmt/core.h>

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
}

void AssetStreamer::request_load(const UUID & uuid)
{
	if (!uuid.is_valid())
		return;

	{
		std::lock_guard<std::mutex> lock(_queueMutex);
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

void AssetStreamer::worker_loop()
{
	while (!_shutdown.load(std::memory_order_acquire)) {
		UUID nextUuid{};
		{ 
			std::unique_lock<std::mutex> lock(_queueMutex);
			// wait until there is a request in queue or shutdown requested
			_cv.wait(lock, [this]() {
				return _shutdown.load(std::memory_order_acquire) || !_requestQueue.empty();
				});
	
			if (_shutdown.load(std::memory_order_acquire)) 
				return;
		
			// get next request from queue
			nextUuid = _requestQueue.front();
			_requestQueue.pop_front();
		}

		// process asset request outside 
		process_asset_request(nextUuid);

		{
			// remove from in flight requests after processing
			std::lock_guard<std::mutex> lock(_queueMutex);
			_inFlightRequests.erase(nextUuid);
		}
	}
}

void AssetStreamer::process_asset_request(const UUID & uuid)
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
		VkFormat fmt = texSettings.sRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
		_rm->queue_main_thread_task([rm = _rm, uuid, name, virtualPath, fmt, texSettings]() {
			rm->load_by_uuid<Texture>(uuid, name, virtualPath, fmt, VK_IMAGE_USAGE_SAMPLED_BIT, texSettings.generateMips);
			});
	}
}
