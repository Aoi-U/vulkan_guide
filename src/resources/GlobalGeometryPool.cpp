#include "GlobalGeometryPool.h"
#include <graphics/VulkanCore.h>
#include <fmt/color.h>

void GlobalGeometryPool::init(VulkanCore* core, ResourceManager* rm)
{
	_core = core;
	_rm = rm;

	size_t maxVertices = (256 * 1024 * 1024) / sizeof(Vertex); // 256 MB for vertices
	size_t maxIndices = (64 * 1024 * 1024) / sizeof(uint32_t); // 64 MB for indices

	_globalVertexBuffer = _rm->load<Buffer>("GlobalVertexBuffer",
		maxVertices * sizeof(Vertex),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		VMA_MEMORY_USAGE_GPU_ONLY
	);

	_globalIndexBuffer = _rm->load<Buffer>("GlobalIndexBuffer",
		maxIndices * sizeof(uint32_t),
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VMA_MEMORY_USAGE_GPU_ONLY
	);

	_vertexBufferAddress = _globalVertexBuffer->get_device_address();

	std::lock_guard<std::mutex> lock(_poolMutex);
	_vertexAllocator.init(static_cast<uint32_t>(maxVertices)); // initialize vertex allocator with max vertices
	_indexAllocator.init(static_cast<uint32_t>(maxIndices)); // initialize index allocator with max indices
}

void GlobalGeometryPool::cleanup()
{
	std::lock_guard<std::mutex> lock(_poolMutex);
	_vertexAllocator.cleanup();
	_indexAllocator.cleanup();

	_globalVertexBuffer = {};
	_globalIndexBuffer = {};
	_vertexBufferAddress = 0;
}

GeometryAllocation GlobalGeometryPool::allocate(uint32_t vertexCount, uint32_t indexCount)
{
	std::lock_guard<std::mutex> lock(_poolMutex);

	uint32_t vOffset = _vertexAllocator.allocate(vertexCount); // get a slot for the requested number of vertices
	if (vOffset == UINT32_MAX) {
		fmt::println("GlobalGeometryPool: out of vertex memory. requested {} vertices", vertexCount);
		return GeometryAllocation{};
	}

	uint32_t iOffset = _indexAllocator.allocate(indexCount);
	if (iOffset == UINT32_MAX) {
		fmt::println("GlobalGeometryPool: out of index memory. requested {} indices", indexCount);
		_vertexAllocator.free(vOffset, vertexCount); // free previously allocated vertex space
		return GeometryAllocation{};
	}

	GeometryAllocation alloc{};
	alloc.vertexCount = vertexCount;
	alloc.indexCount = indexCount;
	alloc.vertexOffset = vOffset;
	alloc.firstIndex = iOffset;
	alloc.vertexBufferAddress = _vertexBufferAddress + (vOffset * sizeof(Vertex));

	return alloc;
}

void GlobalGeometryPool::free(const GeometryAllocation& alloc)
{
	if (alloc.vertexCount == 0 && alloc.indexCount == 0) 
		return;// nothing to free

	std::lock_guard<std::mutex> lock(_poolMutex);

	// free allocated vertex and index ranges back to allocators
	_vertexAllocator.free(alloc.vertexOffset, alloc.vertexCount);
	_indexAllocator.free(alloc.firstIndex, alloc.indexCount);
} 

VkBuffer GlobalGeometryPool::get_vertex_buffer() const
{
	return _globalVertexBuffer->get_buffer();
}

VkBuffer GlobalGeometryPool::get_index_buffer() const
{
	return _globalIndexBuffer->get_buffer();
}
