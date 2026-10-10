#include "GlobalGeometryPool.h"
#include <graphics/VulkanCore.h>
#include <fmt/color.h>

void GlobalGeometryPool::init(VulkanCore* core, ResourceManager* rm)
{
	_core = core;
	_rm = rm;

	size_t maxVertices = (256 * 1024 * 1024) / sizeof(Vertex); // 256 MB for vertices
	size_t maxIndices = (64 * 1024 * 1024) / sizeof(uint32_t); // 64 MB for indices

	_globalVertexBuffer = _rm->preload<Buffer>("GlobalVertexBuffer",
		maxVertices * sizeof(Vertex),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		VMA_MEMORY_USAGE_GPU_ONLY
	);

	_globalIndexBuffer = _rm->preload<Buffer>("GlobalIndexBuffer",
		maxIndices * sizeof(uint32_t),
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VMA_MEMORY_USAGE_GPU_ONLY
	);

	_vertexBufferAddress = _globalVertexBuffer->get_device_address();
}

void GlobalGeometryPool::cleanup()
{
	_globalVertexBuffer = {};
	_globalIndexBuffer = {};
	_nextVertexOffset = 0;
	_nextIndexOffset = 0;
}

GeometryAllocation GlobalGeometryPool::allocate(uint32_t vertexCount, uint32_t indexCount)
{
	GeometryAllocation alloc{};
	alloc.vertexCount = vertexCount;
	alloc.indexCount = indexCount;
	alloc.vertexOffset = _nextVertexOffset;
	alloc.firstIndex = _nextIndexOffset;
	alloc.vertexBufferAddress = _vertexBufferAddress + (_nextVertexOffset * sizeof(Vertex));

	_nextVertexOffset += vertexCount;
	_nextIndexOffset += indexCount;

	return alloc;
}

// TODO: implement free() to allow reusing freed space in the global geometry pool
void GlobalGeometryPool::free(const GeometryAllocation& alloc)
{} 

VkBuffer GlobalGeometryPool::get_vertex_buffer() const
{
	return _globalVertexBuffer->get_buffer();
}

VkBuffer GlobalGeometryPool::get_index_buffer() const
{
	return _globalIndexBuffer->get_buffer();
}
