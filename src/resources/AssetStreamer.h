#pragma once

#include <core/UUID.h>
#include <resources/AssetMetadata.h>
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
	void init(ResourceManager* rm, uint32_t workerCount = 2);
	void cleanup();

	// queue an asset uuid to load
	void request_load(const UUID& uuid);

private:
	void worker_loop();
	void process_asset_request(const UUID& uuid);

	ResourceManager* _rm{ nullptr };
	std::vector<std::thread> _workers;
	std::deque<UUID> _requestQueue;
	std::unordered_set<UUID> _inFlightRequests;

	std::mutex _queueMutex;
	std::condition_variable _cv;
	std::atomic<bool> _shutdown{ false };
};