#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <resources/ResourceManager.h>
#include <resources/Buffer.h>
#include <graphics/utils/vk_types.h>

class VulkanCore;

// allocation of geometry data in the global geometry pool
struct GeometryAllocation
{
	uint32_t vertexOffset;
	uint32_t firstIndex;
	uint32_t vertexCount;
	uint32_t indexCount;
	VkDeviceAddress vertexBufferAddress;
};


// best fit range allocator for managing free ranges of memory in global geometry pool
class RangeAllocator
{
public:
	void init(uint32_t totalCount)
	{
		_ranges.clear();
		if (totalCount > 0) {
			_ranges.push_back({ 0, totalCount });
		}
	}

	void cleanup()
	{
		_ranges.clear();
	}

	uint32_t allocate(uint32_t count) 
	{
		if (count == 0)
			return 0; // nothing to allocate

		size_t bestFitIndex = SIZE_MAX; // index of best fit range
		uint32_t minExcess = UINT32_MAX; // current minimum excess space 

		// search for best fit to minimize fragmentation
		for (size_t i = 0; i < _ranges.size(); i++) {
			if (_ranges[i].count >= count) {
				uint32_t excess = _ranges[i].count - count;
				if (excess < minExcess) {
					minExcess = excess;
					bestFitIndex = i;

					if (excess == 0) // perfect fit
						break;
				}
			}
		}

		if (bestFitIndex == SIZE_MAX) { // out of available memory
			return UINT32_MAX;
		}

		// allocate from best fit range
		uint32_t allocatedOffset = _ranges[bestFitIndex].offset;
		_ranges[bestFitIndex].offset += count;
		_ranges[bestFitIndex].count -= count;

		if (_ranges[bestFitIndex].count == 0) { // remove empty range
			_ranges.erase(_ranges.begin() + bestFitIndex);
		}

		return allocatedOffset;
	}

	void free(uint32_t offset, uint32_t count) 
	{
		if (count == 0)
			return; // nothing to free

		for (const FreeRange& r : _ranges) { // check for overlap with existing free ranges
			if (std::max(r.offset, offset) < std::min(r.offset + r.count, offset + count)) { // overlap detected
				return;
			}
		}

		size_t insertIndex = 0;
		while (insertIndex < _ranges.size() && _ranges[insertIndex].offset < offset) { // find insertion point to keep ranges sorted
			insertIndex++;
		}
		_ranges.insert(_ranges.begin() + insertIndex, { offset, count }); // insert new free range

		if (insertIndex + 1 < _ranges.size()) { // see if next block is contiguous with freed block
			if (_ranges[insertIndex].offset + _ranges[insertIndex].count == _ranges[insertIndex + 1].offset) { // if end of current block is contiguous with start of next block, merge them
				_ranges[insertIndex].count += _ranges[insertIndex + 1].count;
				_ranges.erase(_ranges.begin() + insertIndex + 1); 
			}
		}

		if (insertIndex > 0) { // see if previous block is contiguous with freed block
			if (_ranges[insertIndex - 1].offset + _ranges[insertIndex - 1].count == _ranges[insertIndex].offset) { // if end of previous block is contiguous with start of current block, merge them
				_ranges[insertIndex - 1].count += _ranges[insertIndex].count;
				_ranges.erase(_ranges.begin() + insertIndex);
			}
		}
	}

private:
	struct FreeRange
	{
		uint32_t offset{ 0 }; // where the free range starts in the buffer
		uint32_t count{ 0 }; // number of elements free in the range
	};

	std::vector<FreeRange> _ranges; // list of free buffer ranges in the global pool
};

class GlobalGeometryPool
{
public:
	void init(VulkanCore* core, ResourceManager* rm);
	void cleanup();

	GeometryAllocation allocate(uint32_t vertexCount, uint32_t indexCount);
	void free(const GeometryAllocation& alloc);

	VkBuffer get_vertex_buffer() const;
	VkBuffer get_index_buffer() const;
	VkDeviceAddress get_vertex_buffer_address() const { return _vertexBufferAddress; }

private:
	VulkanCore* _core{ nullptr };
	ResourceManager* _rm{ nullptr };
	ResourceHandle<Buffer> _globalVertexBuffer;
	ResourceHandle<Buffer> _globalIndexBuffer;
	VkDeviceAddress _vertexBufferAddress{ 0 };

	std::mutex _poolMutex;
	RangeAllocator _vertexAllocator;
	RangeAllocator _indexAllocator;
};