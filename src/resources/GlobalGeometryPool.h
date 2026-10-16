#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <mutex>
#include <vector>
#include <deque>
#include <resources/ResourceManager.h>
#include <resources/Buffer.h>
#include <graphics/utils/vk_types.h>
#include <offsetAllocator.hpp>

class VulkanCore;
class StagingUploader;

// allocation of geometry data 
struct GeometryAllocation
{
	uint32_t pageIndex{ UINT32_MAX };
	OffsetAllocator::Allocation vertexAlloc{};
	OffsetAllocator::Allocation indexAlloc{};
	VkBuffer vertexBuffer{ VK_NULL_HANDLE };
	VkDeviceAddress vertexBufferAddress{ 0 }; // paged bda + byte offset
	uint32_t vertexOffset{ 0 }; // element offset inside vertex page
	uint32_t firstIndex{ 0 }; // element offset inside global index buffer
	uint32_t vertexCount{ 0 };
	uint32_t indexCount{ 0 };

	bool is_valid() const noexcept
	{
		return vertexBufferAddress != 0;
	}

	bool has_vertex_alloc() const noexcept
	{
		return vertexAlloc.offset != OffsetAllocator::Allocation::NO_SPACE && vertexAlloc.metadata != OffsetAllocator::Allocation::NO_SPACE;
	}

	bool has_index_alloc() const noexcept
	{
		return indexAlloc.offset != OffsetAllocator::Allocation::NO_SPACE && indexAlloc.metadata != OffsetAllocator::Allocation::NO_SPACE;
	}
};

struct GeometryPoolMetrics
{
	uint32_t activeVertexPages{ 0 };
	uint64_t totalVertexCapacityBytes{ 0 };
	uint64_t usedVertexBytes{ 0 };
	uint64_t largestFreeVertexBlockBytes{ 0 };
	uint64_t totalIndexCapacityBytes{ 0 };
	uint64_t usedIndexBytes{ 0 };
};

class GlobalGeometryPool
{
public:
	static constexpr size_t VERTEX_PAGE_SIZE_BYTES = 64 * 1024 * 1024; // 64mb
	static constexpr size_t INDEX_POOL_SIZE_BYTES = 64 * 1024 * 1024; // 64mb
	static constexpr uint32_t FRAME_DELAY = 2;

	void init(VulkanCore* core, ResourceManager* rm);
	void cleanup();

	GeometryAllocation allocate(uint32_t vertexCount, uint32_t indexCount);
	void free(const GeometryAllocation& alloc);
	
	void process_deferred_frees(uint64_t currentFrame);

	VkBuffer get_vertex_buffer(uint32_t pageIndex = 0) const;
	VkBuffer get_index_buffer() const;
	VkDeviceAddress get_vertex_buffer_address(uint32_t pageIndex = 0) const;

	GeometryPoolMetrics get_metrics() const;

	bool run_test();

private:
	struct GeometryPage
	{
		uint32_t pageIndex{ 0 };
		ResourceHandle<Buffer> buffer;
		VkDeviceAddress baseAddress{ 0 };
		uint32_t capacityVertices{ 0 };
		uint32_t activeAllocations{ 0 };
		std::unique_ptr<OffsetAllocator::Allocator> allocator;
		bool retired{ false };
	};

	struct PendingFree
	{
		GeometryAllocation allocation;
		uint64_t frameFreed{ 0 };
	};

	uint32_t create_vertex_page(uint32_t minVertexCapacity = 0);
	void free_immediate_locked(const GeometryAllocation& alloc);

	VulkanCore* _core{ nullptr };
	ResourceManager* _rm{ nullptr };

	std::vector<GeometryPage> _vertexPages;
	ResourceHandle<Buffer> _globalIndexBuffer;
	std::unique_ptr<OffsetAllocator::Allocator> _indexAllocator;
	uint32_t _maxIndices{ 0 };

	std::deque<PendingFree> _deferredFrees;
	uint64_t _currentFrame{ 0 };
	uint32_t _pageCounter{ 0 };

	mutable std::mutex _poolMutex;
};