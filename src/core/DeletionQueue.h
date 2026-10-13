#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <vector>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>
#include <cstdint>

struct DeletionBatch
{
  std::vector<std::function<void()>> callbacks;
  std::vector<VkImageView> imageViews;
  std::vector<VkSampler> samplers;
  std::vector<std::pair<VkImage, VmaAllocation>> images;
  std::vector<std::pair<VkBuffer, VmaAllocation>> buffers;
  std::vector<VkShaderModule> shaderModules;
  std::vector<VkPipeline> pipelines;
  std::vector<VkPipelineLayout> pipelineLayouts;
  std::vector<VkDescriptorSetLayout> descriptorSetLayouts;
  std::vector<VkDescriptorPool> descriptorPools;

  void flush(VkDevice device, VmaAllocator allocator); // flushes deletion batch
  bool empty() const; // returns true if the deletion batch is empty
  void clear(); // clears the deletion batch without flushing
};

class DeletionQueue
{
public:
  static constexpr uint32_t FRAME_DELAY = 2;

  DeletionQueue() = default;
  ~DeletionQueue() = default;

  DeletionQueue(const DeletionQueue&) = delete;
  DeletionQueue& operator=(const DeletionQueue&) = delete;

  void init(VkDevice device, VmaAllocator allocator, uint32_t frameDelay = FRAME_DELAY);

  void push(VkBuffer buffer, VmaAllocation allocation);
  void push(VkImage image, VmaAllocation allocation);
  void push(VkImageView imageView);
  void push(VkSampler sampler);
  void push(VkShaderModule shaderModule);
  void push(VkPipeline pipeline);
  void push(VkPipelineLayout pipelineLayout);
  void push(VkDescriptorSetLayout descriptorLayout);
  void push(VkDescriptorPool pool);

  void push_function(std::function<void()>&& func);

  void next_frame(uint64_t frameNumber);

  void flush_retired(uint64_t completedFrameNumber);

  void flush_all(); // call only after vkDeviceWaitIdle

private:
  VkDevice _device{ VK_NULL_HANDLE };
  VmaAllocator _allocator{ VK_NULL_HANDLE };
  uint32_t _frameDelay{ FRAME_DELAY };

  std::mutex _mutex;
  DeletionBatch _currentBatch;
  std::deque<std::pair<uint64_t, DeletionBatch>> _pendingBatches;
};