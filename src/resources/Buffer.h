#pragma once

#include <resources/ResourceManager.h>
#include <core/vk_types.h>
#include <span>
#include <vector>

class VulkanCore;

class Buffer : public Resource
{
public:
	Buffer() // default constructor for empty buffer
    : Resource(""), _size(0), _usage(0), _memoryUsage(VMA_MEMORY_USAGE_UNKNOWN), _allocFlags(0) {}
  Buffer(const std::string& id, size_t size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage, VmaAllocationCreateFlags allocFlags = 0); // constructor for empty buffer
	Buffer(const std::string& id, std::span<const uint8_t> initialData, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage); // constructor for buffer with initial data

  Buffer(Buffer&& other) noexcept;
  Buffer& operator=(Buffer&& other) noexcept;
  ~Buffer() override;

  void* map(VulkanCore* core);
  void unmap(VulkanCore* core);
  void copy_data(const void* data, size_t size, size_t offset = 0);
  void* get_mapped_data() const { return _mappedData; }

  bool resize(VulkanCore* core, size_t newSize, bool preserveData = false);

  VkDeviceAddress get_device_address(VulkanCore* core) const;

  VkBuffer get_buffer() const { return _buffer; }
  size_t get_size() const { return _size; }
  VmaAllocation get_allocation() const { return _allocation; }
  VkBufferUsageFlags get_usage() const { return _usage; }

protected:
	bool doLoad(VulkanCore* core) override;
	bool doUnload(VulkanCore* core) override;

private:
  VulkanCore* _core{ nullptr };

  size_t _size;
  VkBufferUsageFlags _usage;
  VmaMemoryUsage _memoryUsage;
  VmaAllocationCreateFlags _allocFlags;
  std::vector<uint8_t> _initialData;

  VkBuffer _buffer{ VK_NULL_HANDLE };
  VmaAllocation _allocation{ VK_NULL_HANDLE };
  VmaAllocationInfo _allocInfo{};
  void* _mappedData{ nullptr };
};