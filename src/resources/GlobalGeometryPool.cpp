#include "GlobalGeometryPool.h"

void GlobalGeometryPool::init(VulkanCore* core, ResourceManager* rm)
{
	_core = core;
	_rm = rm;
	_currentFrame = 0;
	_pageCounter = 0;

	std::lock_guard<std::mutex> lock(_poolMutex);
	_vertexPages.clear();
	_deferredFrees.clear();

	create_vertex_page(); // allocate initial 64mb vertex page 0

	_maxIndices = static_cast<uint32_t>(INDEX_POOL_SIZE_BYTES / sizeof(uint32_t));
	_globalIndexBuffer = _rm->load<Buffer>(
		"GlobalIndexBuffer",
		_maxIndices * sizeof(uint32_t),
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VMA_MEMORY_USAGE_GPU_ONLY
	);
	_indexAllocator = std::make_unique<OffsetAllocator::Allocator>(_maxIndices);
}

void GlobalGeometryPool::cleanup()
{
	std::lock_guard<std::mutex> lock(_poolMutex);
	_deferredFrees.clear();

	for (auto& page : _vertexPages) {
		page.allocator.reset();
		page.buffer = {};
		page.baseAddress = 0;
	}
	_vertexPages.clear();

	_indexAllocator.reset();
	_globalIndexBuffer = {};
}

GeometryAllocation GlobalGeometryPool::allocate(uint32_t vertexCount, uint32_t indexCount)
{
	if (vertexCount == 0 || indexCount == 0) {
		return GeometryAllocation{};
	}

	std::lock_guard<std::mutex> lock(_poolMutex);

	// allocate indices from global index buffer
	OffsetAllocator::Allocation idxAlloc = _indexAllocator->allocate(indexCount);
	if (idxAlloc.offset == OffsetAllocator::Allocation::NO_SPACE) {
		fmt::println("GlobalGeometryPool: out of index memory (requested {} indices)", indexCount);
		return GeometryAllocation{};
	}

	// try allocating vertices from existing active page
	uint32_t chosenPage = UINT32_MAX;
	OffsetAllocator::Allocation vertAlloc{};

	for (uint32_t i = 0; i < static_cast<uint32_t>(_vertexPages.size()); i++) {
		if (_vertexPages[i].retired)
			continue;

			vertAlloc = _vertexPages[i].allocator->allocate(vertexCount);
			if (vertAlloc.offset != OffsetAllocator::Allocation::NO_SPACE) {
				chosenPage = i;
				break;
			}
	}

	// if all existing pages are full, allocate new page
	if (vertAlloc.offset == OffsetAllocator::Allocation::NO_SPACE) {
		chosenPage = create_vertex_page(vertexCount);
		vertAlloc = _vertexPages[chosenPage].allocator->allocate(vertexCount);
		if (vertAlloc.offset == OffsetAllocator::Allocation::NO_SPACE) {
			fmt::println("GlobalGeometryPool: failed to allocate {} vertices on new page {}", vertexCount, chosenPage);

			// free index allocation since we failed to allocate vertices
			_indexAllocator->free(idxAlloc);
			return GeometryAllocation{};
		}
	}

	// create geometry allocation result
	GeometryPage& page = _vertexPages[chosenPage];
	page.activeAllocations++;

	GeometryAllocation result{};
	result.pageIndex = chosenPage;
	result.vertexAlloc = vertAlloc;
	result.indexAlloc = idxAlloc;
	result.vertexBuffer = page.buffer.is_valid() ? page.buffer->get_buffer() : VK_NULL_HANDLE;
	result.vertexOffset = vertAlloc.offset;
	result.firstIndex = idxAlloc.offset;
	result.vertexCount = vertexCount;
	result.indexCount = indexCount;
	result.vertexBufferAddress = page.baseAddress + (static_cast<VkDeviceAddress>(vertAlloc.offset) * sizeof(Vertex));
	
	return result;
}

void GlobalGeometryPool::free(const GeometryAllocation& alloc)
{
	if (alloc.vertexCount == 0 && alloc.indexCount == 0)
		return;

	if (!alloc.has_vertex_alloc() && !alloc.has_index_alloc())
		return;

	std::lock_guard<std::mutex> lock(_poolMutex);
	_deferredFrees.push_back(PendingFree{ alloc, _currentFrame });
}

void GlobalGeometryPool::process_deferred_frees(uint64_t currentFrame)
{
	std::lock_guard<std::mutex> lock(_poolMutex);
	_currentFrame = currentFrame;

	while (!_deferredFrees.empty()) {
		const PendingFree& front = _deferredFrees.front();
		if (currentFrame < front.frameFreed + FRAME_DELAY) {
			break; // not ready to free yet
		}
		free_immediate_locked(front.allocation);
		_deferredFrees.pop_front();
	}
}

VkBuffer GlobalGeometryPool::get_vertex_buffer(uint32_t pageIndex) const
{
	std::lock_guard<std::mutex> lock(_poolMutex);
	if (pageIndex < _vertexPages.size() && !_vertexPages[pageIndex].retired && _vertexPages[pageIndex].buffer.is_valid()) {
		return _vertexPages[pageIndex].buffer->get_buffer();
	}
	return VK_NULL_HANDLE;
}

VkBuffer GlobalGeometryPool::get_index_buffer() const
{
	return _globalIndexBuffer.is_valid() ? _globalIndexBuffer->get_buffer() : VK_NULL_HANDLE;
}

VkDeviceAddress GlobalGeometryPool::get_vertex_buffer_address(uint32_t pageIndex) const
{
	std::lock_guard<std::mutex> lock(_poolMutex);
	if (pageIndex < _vertexPages.size() && !_vertexPages[pageIndex].retired) {
		return _vertexPages[pageIndex].baseAddress;
	}
	return 0;
}

GeometryPoolMetrics GlobalGeometryPool::get_metrics() const
{
	std::lock_guard<std::mutex> lock(_poolMutex);
	GeometryPoolMetrics stats{};

	for (const auto& page : _vertexPages) {
		if (page.retired)
			continue;

		stats.activeVertexPages++;
		uint64_t pageBytes = static_cast<uint64_t>(page.capacityVertices) * sizeof(Vertex);
		auto report = page.allocator->storageReport();
		uint64_t freeBytes = static_cast<uint64_t>(report.totalFreeSpace) * sizeof(Vertex);

		stats.totalVertexCapacityBytes += pageBytes;
		stats.usedVertexBytes += (pageBytes - freeBytes);
		stats.largestFreeVertexBlockBytes = std::max(stats.largestFreeVertexBlockBytes, static_cast<uint64_t>(report.largestFreeRegion) * sizeof(Vertex));
	}

	auto idxReport = _indexAllocator->storageReport();
	stats.totalIndexCapacityBytes = static_cast<uint64_t>(_maxIndices) * sizeof(uint32_t);
	stats.usedIndexBytes = static_cast<uint64_t>(_maxIndices - idxReport.totalFreeSpace) * sizeof(uint32_t);

	return stats;
}

bool GlobalGeometryPool::run_test()
{
	fmt::println("GlobalGeometryPool: starting phase 2 stress test (1,000 random mesh allocations/frees)...");
	std::mt19937 rng(1337);
	std::uniform_int_distribution<uint32_t> vertDist(64, 32768);
	std::uniform_int_distribution<uint32_t> idxDist(128, 65536);

	std::vector<GeometryAllocation> activeAllocs;
	activeAllocs.reserve(1000);

	uint64_t simulatedFrame = _currentFrame + 1;

	for (uint32_t i = 0; i < 1000; i++) {
		uint32_t vCount = vertDist(rng);
		uint32_t iCount = idxDist(rng);

		GeometryAllocation alloc = allocate(vCount, iCount);
		if (!alloc.is_valid()) {
			fmt::println("GlobalGeometryPool stress test FAILED at allocation {}", i);
			return false;
		}
		activeAllocs.push_back(alloc);

		// randomly free ~40% of existing allocations to induce heavy fragmentation pressure
		if (activeAllocs.size() > 25 && (i % 3 == 0)) {
			size_t victim = rng() % activeAllocs.size();
			free(activeAllocs[victim]);
			activeAllocs[victim] = activeAllocs.back();
			activeAllocs.pop_back();
		}

		if (i % 50 == 0) {
			simulatedFrame++;
			process_deferred_frees(simulatedFrame);
		}
	}

	// free all remaining allocations and advance frames past FRAME_DELAY
	for (const auto& alloc : activeAllocs) {
		free(alloc);
	}
	activeAllocs.clear();

	simulatedFrame += (FRAME_DELAY + 2);
	process_deferred_frees(simulatedFrame);

	GeometryPoolMetrics telemetry = get_metrics();
	bool passed = (telemetry.usedVertexBytes == 0 && telemetry.usedIndexBytes == 0 && telemetry.activeVertexPages == 1);

	fmt::println("GlobalGeometryPool stress test {}: active pages = {}, used vertex bytes = {}, used index bytes = {}",
		passed ? "PASSED" : "FAILED",
		telemetry.activeVertexPages,
		telemetry.usedVertexBytes,
		telemetry.usedIndexBytes);

	return passed;
}

uint32_t GlobalGeometryPool::create_vertex_page(uint32_t minVertexCapacity)
{
	uint32_t defaultVertices = static_cast<uint32_t>(VERTEX_PAGE_SIZE_BYTES / sizeof(Vertex));
	uint32_t pageVertices = std::max(defaultVertices, minVertexCapacity);
	size_t pageByteSize = static_cast<size_t>(pageVertices) * sizeof(Vertex);

	// reuse retired page slot if available
	uint32_t slotIndex = static_cast<uint32_t>(_vertexPages.size());
	for (uint32_t i = 0; i < static_cast<uint32_t>(_vertexPages.size()); i++) {
		if (_vertexPages[i].retired) {
			slotIndex = i;
			break;
		}
	}

	// create new page
	std::string pageName = fmt::format("GlobalVertexBuffer_Page_{}", _pageCounter++);
	ResourceHandle<Buffer> pageBuf = _rm->load<Buffer>(pageName, pageByteSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

	GeometryPage page{};
	page.pageIndex = slotIndex;
	page.buffer = pageBuf;
	page.baseAddress = pageBuf.is_valid() ? pageBuf->get_device_address() : 0;
	page.capacityVertices = pageVertices;
	page.activeAllocations = 0;
	page.allocator = std::make_unique<OffsetAllocator::Allocator>(pageVertices);
	page.retired = false;

	if (slotIndex < _vertexPages.size()) {
		_vertexPages[slotIndex] = std::move(page);
	}
	else {
		_vertexPages.push_back(std::move(page));
	}

	fmt::println("GlobalGeometryPool: Allocated Vertex Page {} ({:.1f} MB, {} max vertices)", slotIndex, static_cast<double>(pageByteSize) / (1024 * 1024), pageVertices);
	
	return slotIndex;
}

void GlobalGeometryPool::free_immediate_locked(const GeometryAllocation& alloc)
{
	if (alloc.has_index_alloc() && _indexAllocator) {
		_indexAllocator->free(alloc.indexAlloc);
	}

	if (alloc.has_vertex_alloc() && alloc.pageIndex < _vertexPages.size()) {
		GeometryPage& page = _vertexPages[alloc.pageIndex];
		if (!page.retired) {
			page.allocator->free(alloc.vertexAlloc);
			if (page.activeAllocations > 0) {
				page.activeAllocations--;
			}

			// reclaim page when fully empty
			if (alloc.pageIndex > 0 && page.activeAllocations == 0) {
				fmt::println("GlobalGeometryPool: page {} fully empty, releasing back to vma", alloc.pageIndex);
				page.allocator.reset();
				page.buffer = {}; // ResourceHandle queues buffer for deletion
				page.baseAddress = 0;
				page.retired = true;
			}
		}
	}
}
