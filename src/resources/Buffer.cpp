#include "resources/Buffer.h"

#include <graphics/VulkanCore.h>
#include <cstring>
#include <algorithm>

Buffer::Buffer(const std::string& id, size_t size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage, VmaAllocationCreateFlags allocFlags)
	: Resource(id), _size(size), _usage(usage), _memoryUsage(memoryUsage), _allocFlags(allocFlags)
{}

Buffer::Buffer(const std::string & id, std::span<const uint8_t> initialData, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage)
	: Resource(id), _size(initialData.size()), _usage(usage), _memoryUsage(memoryUsage), _allocFlags(0), _initialData(initialData.begin(), initialData.end())
{}

Buffer::Buffer(Buffer&& other) noexcept
	: Resource(std::move(other)), _size(other._size), _usage(other._usage),
	_memoryUsage(other._memoryUsage), _allocFlags(other._allocFlags),
	_initialData(std::move(other._initialData)), _buffer(other._buffer),
	_allocation(other._allocation), _allocInfo(other._allocInfo),
	_mappedData(other._mappedData), _core(other._core)
{
	other._buffer = VK_NULL_HANDLE;
	other._allocation = VK_NULL_HANDLE;
	other._mappedData = nullptr;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
	if (this != &other) {
		if (_buffer != VK_NULL_HANDLE && _core != nullptr) {
			_core->globalDeletionQueue.push(_buffer, _allocation);
		}
		Resource::operator=(std::move(other));
		_size = other._size; _usage = other._usage; _memoryUsage = other._memoryUsage;
		_allocFlags = other._allocFlags; _initialData = std::move(other._initialData);
		_buffer = other._buffer; _allocation = other._allocation;
		_allocInfo = other._allocInfo; _mappedData = other._mappedData; _core = other._core;

		other._buffer = VK_NULL_HANDLE; other._allocation = VK_NULL_HANDLE; other._mappedData = nullptr;
	}
	return *this;
}

Buffer::~Buffer() {
	if (_buffer != VK_NULL_HANDLE && _core != nullptr) {
		_core->globalDeletionQueue.push(_buffer, _allocation);
	}
}

void Buffer::destroy_immediate(VulkanCore* core)
{
	if (_buffer != VK_NULL_HANDLE) {
		vmaDestroyBuffer(core->get_allocator(), _buffer, _allocation);
		_buffer = VK_NULL_HANDLE;
		_allocation = VK_NULL_HANDLE;
		_mappedData = nullptr;
	}
}

void* Buffer::map(VulkanCore * core)
{
	if (!_mappedData && _allocation) {
		vmaMapMemory(core->get_allocator(), _allocation, &_mappedData);
	}

	return _mappedData;
}

void Buffer::unmap(VulkanCore* core)
{
	if (_mappedData && _allocation) {
		vmaUnmapMemory(core->get_allocator(), _allocation);
		_mappedData = nullptr;
	}
}

void Buffer::copy_data(const void* data, size_t size, size_t offset)
{
	if (_mappedData) {
		memcpy(static_cast<uint8_t*>(_mappedData) + offset, data, size);
	}
}

bool Buffer::resize(VulkanCore * core, size_t newSize, bool preserveData)
{
	if (is_loaded() && newSize <= _size)
		return true; // no resize required

	size_t targetSize = std::max(newSize, static_cast<size_t>(_size * 1.5)); 

	if (!is_loaded()) {
		_size = targetSize;
		return load(core);
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

	// create new buffer
	if (vmaCreateBuffer(core->get_allocator(), &bufferInfo, &vmaAllocInfo, &newBuffer, &newAllocation, &newAllocInfo) != VK_SUCCESS) {
		fmt::println("Buffer: failed to create buffer for resize: {}", get_id());
		return false;
	}

	// copy data from old buffer to new buffer if requested
	if (preserveData && _size > 0) {
		if (_mappedData && newAllocInfo.pMappedData) { 
			memcpy(newAllocInfo.pMappedData, _mappedData, _size);
		}
		else {
			core->immediate_submit([&](VkCommandBuffer cmd) {
				VkBufferCopy copyRegion{};
				copyRegion.srcOffset = 0;
				copyRegion.dstOffset = 0;
				copyRegion.size = _size;
				vkCmdCopyBuffer(cmd, _buffer, newBuffer, 1, &copyRegion);
				});
		}
	}

	destroy_immediate(core);
	
	_size = targetSize;
	_buffer = newBuffer;
	_allocation = newAllocation;
	_allocInfo = newAllocInfo;
	_mappedData = _allocInfo.pMappedData;

	return true;
}

VkDeviceAddress Buffer::get_device_address(VulkanCore* core) const
{
	if (_buffer == VK_NULL_HANDLE)
		return 0;

	VkBufferDeviceAddressInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
		.buffer = _buffer
	};

	return vkGetBufferDeviceAddress(core->get_device(), &info);
}

bool Buffer::doLoad(VulkanCore* core)
{
	_core = core;

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

	if (vmaCreateBuffer(core->get_allocator(), &bufferInfo, &vmaAllocInfo, &_buffer, &_allocation, &_allocInfo) != VK_SUCCESS) {
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
			Buffer staging(get_id() + "_staging", _initialData, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
			staging.load(core);
			core->immediate_submit([&](VkCommandBuffer cmd) {
				VkBufferCopy copyRegion{};
				copyRegion.srcOffset = 0;
				copyRegion.dstOffset = 0;
				copyRegion.size = _initialData.size();
				vkCmdCopyBuffer(cmd, staging.get_buffer(), _buffer, 1, &copyRegion);
				});
			staging.destroy_immediate(core);
		}
		_initialData.clear();
	}

	return true;
}

bool Buffer::doUnload(VulkanCore* core)
{
	if (_buffer != VK_NULL_HANDLE) {
		core->globalDeletionQueue.push(_buffer, _allocation);
		_buffer = VK_NULL_HANDLE;
		_allocation = VK_NULL_HANDLE;
		_mappedData = nullptr;
	}

	return true;
}
