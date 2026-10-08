#include "resources/Buffer.h"

#include <graphics/VulkanCore.h>
#include <cstring>
#include <algorithm>

Buffer::Buffer(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, size_t size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage, VmaAllocationCreateFlags allocFlags)
	: Resource(id, core, resourceManager), _size(size), _usage(usage), _memoryUsage(memoryUsage), _allocFlags(allocFlags)
{}

Buffer::Buffer(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, std::span<const uint8_t> initialData, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage)
	: Resource(id, core, resourceManager), _size(initialData.size()), _usage(usage), _memoryUsage(memoryUsage), _allocFlags(0), _initialData(initialData.begin(), initialData.end())
{}

void* Buffer::map()
{
	if (!_mappedData && _allocation) {
		vmaMapMemory(_core->get_allocator(), _allocation, &_mappedData);
	}

	return _mappedData;
}

void Buffer::unmap()
{
	if (_mappedData && _allocation) {
		vmaUnmapMemory(_core->get_allocator(), _allocation);
		_mappedData = nullptr;
	}
}

void Buffer::copy_data(const void* data, size_t size, size_t offset)
{
	if (_mappedData) {
		memcpy(static_cast<uint8_t*>(_mappedData) + offset, data, size);
	}
}

bool Buffer::resize(size_t newSize, bool preserveData)
{
	if (is_loaded() && newSize <= _size)
		return true; // no resize required

	size_t targetSize = std::max(newSize, static_cast<size_t>(_size * 1.5)); 

	if (!is_loaded()) {
		_size = targetSize;
		return load();
	}

	VkBuffer newBuffer = VK_NULL_HANDLE;
	VmaAllocation newAllocation = VK_NULL_HANDLE;
	VmaAllocationInfo newAllocInfo = {};

	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = targetSize;
	bufferInfo.usage = _usage;

	VmaAllocationCreateInfo vmaAllocInfo = {};
	vmaAllocInfo.usage = _memoryUsage;
	vmaAllocInfo.flags = _allocFlags;
	if (_memoryUsage == VMA_MEMORY_USAGE_CPU_TO_GPU || _memoryUsage == VMA_MEMORY_USAGE_CPU_ONLY) {
		vmaAllocInfo.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
	}

	//ResourceHandle<Buffer> newBufferHandle = _resourceManager->load<Buffer>(get_id() + "_resized", _core, _resourceManager, targetSize, _usage, _memoryUsage, _allocFlags);

	// create new buffer
	if (vmaCreateBuffer(_core->get_allocator(), &bufferInfo, &vmaAllocInfo, &newBuffer, &newAllocation, &newAllocInfo) != VK_SUCCESS) {
		fmt::println("Buffer: failed to create buffer for resize: {}", get_id());
		return false;
	}

	// copy data from old buffer to new buffer if requested
	if (preserveData && _size > 0) {
		if (_mappedData && newAllocInfo.pMappedData) { 
			memcpy(newAllocInfo.pMappedData, _mappedData, _size);
		}
		else {
			_core->immediate_submit([&](VkCommandBuffer cmd) {
				VkBufferCopy copyRegion{};
				copyRegion.srcOffset = 0;
				copyRegion.dstOffset = 0;
				copyRegion.size = _size;
				vkCmdCopyBuffer(cmd, _buffer, newBuffer, 1, &copyRegion);
				});
		}
	}

	//destroy_immediate();
	unload();
	
	_size = targetSize;
	_buffer = newBuffer;
	_allocation = newAllocation;
	_allocInfo = newAllocInfo;
	_mappedData = _allocInfo.pMappedData;

	return true;
}

VkDeviceAddress Buffer::get_device_address() const
{
	if (_buffer == VK_NULL_HANDLE)
		return 0;

	VkBufferDeviceAddressInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
		.buffer = _buffer
	};

	return vkGetBufferDeviceAddress(_core->get_device(), &info);
}

bool Buffer::doLoad()
{
	if (_size == 0) return false; 

	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = _size;
	bufferInfo.usage = _usage;
	if (!_initialData.empty() && _memoryUsage == VMA_MEMORY_USAGE_GPU_ONLY) {
		bufferInfo.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT; // staging buffer
	}

	VmaAllocationCreateInfo vmaAllocInfo = {};
	vmaAllocInfo.usage = _memoryUsage;
	vmaAllocInfo.flags = _allocFlags;
	if (_memoryUsage == VMA_MEMORY_USAGE_CPU_TO_GPU || _memoryUsage == VMA_MEMORY_USAGE_CPU_ONLY) {
		vmaAllocInfo.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT; 
	}

	if (vmaCreateBuffer(_core->get_allocator(), &bufferInfo, &vmaAllocInfo, &_buffer, &_allocation, &_allocInfo) != VK_SUCCESS) {
		fmt::println("Buffer: failed to create buffer for: {}", get_id());
		return false;
	}
	_mappedData = _allocInfo.pMappedData;

	// if there is initial data, copy it to the buffer
	if (!_initialData.empty()) {
		if (_mappedData) { // if the buffer is mappable, copy directly
			memcpy(_mappedData, _initialData.data(), _initialData.size());
		}
		else { // if the buffer is not mappable, use a staging buffer
			//Buffer staging(get_id() + "_staging", _core, _initialData, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
			//staging.load();
			ResourceHandle<Buffer> staging = _resourceManager->load<Buffer>(get_id() + "_staging", _initialData, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
			_core->immediate_submit([&](VkCommandBuffer cmd) {
				VkBufferCopy copyRegion{};
				copyRegion.srcOffset = 0;
				copyRegion.dstOffset = 0;
				copyRegion.size = _initialData.size();
				vkCmdCopyBuffer(cmd, staging->get_buffer(), _buffer, 1, &copyRegion);
				});
		}
		_initialData.clear();
	}

	return true;
}

bool Buffer::doUnload()
{
	if (_buffer != VK_NULL_HANDLE) {
		_core->globalDeletionQueue.push(_buffer, _allocation);
		_buffer = VK_NULL_HANDLE;
		_allocation = VK_NULL_HANDLE;
		_mappedData = nullptr;
	}

	return true;
}
