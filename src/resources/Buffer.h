#pragma once

#include <resources/ResourceManager.h>
#include <core/vk_types.h>
#include <span>
#include <vector>

class Buffer : public Resource
{
public:
	Buffer() // default constructor for empty buffer
    : Resource("", nullptr, nullptr), _size(0), _usage(0), _memoryUsage(VMA_MEMORY_USAGE_UNKNOWN), _allocFlags(0) {}
  Buffer(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, size_t size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage, VmaAllocationCreateFlags allocFlags = 0); // constructor for empty buffer                                                                  M
	Buffer(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, std::span<const uint8_t> initialData, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage); // constructor for buffer with initial data

  ~Buffer() override { Resource::unload(); }

  void destroy_immediate();

  void* map();
  void unmap();
  void copy_data(const void* data, size_t size, size_t offset = 0);
  void* get_mapped_data() const { return _mappedData; }

  bool resize(size_t newSize, bool preserveData = false);

  VkDeviceAddress get_device_address() const;

  VkBuffer get_buffer() const { return _buffer; }
  size_t get_size() const { return _size; }
  VmaAllocation get_allocation() const { return _allocation; }
  VkBufferUsageFlags get_usage() const { return _usage; }

protected:
	bool doLoad() override;
	bool doUnload() override;

private:
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