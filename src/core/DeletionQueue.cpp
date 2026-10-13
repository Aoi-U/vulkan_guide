#include "DeletionQueue.h"

void DeletionBatch::flush(VkDevice device, VmaAllocator allocator)
{
	for (auto it = callbacks.begin(); it != callbacks.end(); it++) {
		if (*it) {
			(*it)();
		}
	}
	callbacks.clear();

	for (auto it = imageViews.rbegin(); it != imageViews.rend(); it++) {
		if (*it != VK_NULL_HANDLE) {
			vkDestroyImageView(device, *it, nullptr);
		}
	}
	imageViews.clear();

	for (auto it = samplers.rbegin(); it != samplers.rend(); it++) {
		if (*it != VK_NULL_HANDLE) {
			vkDestroySampler(device, *it, nullptr);
		}
	}
	samplers.clear();

	for (auto it = images.rbegin(); it != images.rend(); it++) {
		if (it->first != VK_NULL_HANDLE) {
			vmaDestroyImage(allocator, it->first, it->second);
		}
	}
	images.clear();

	for (auto it = buffers.rbegin(); it != buffers.rend(); it++) {
		if (it->first != VK_NULL_HANDLE) {
			vmaDestroyBuffer(allocator, it->first, it->second);
		}
	}
	buffers.clear();

	for (auto it = shaderModules.rbegin(); it != shaderModules.rend(); it++) {
		if (*it != VK_NULL_HANDLE) {
			vkDestroyShaderModule(device, *it, nullptr);
		}
	}
	shaderModules.clear();

	for (auto it = pipelines.rbegin(); it != pipelines.rend(); it++) {
		if (*it != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, *it, nullptr);
		}
	}
	pipelines.clear();

	for (auto it = pipelineLayouts.rbegin(); it != pipelineLayouts.rend(); it++) {
		if (*it != VK_NULL_HANDLE) {
			vkDestroyPipelineLayout(device, *it, nullptr);
		}
	}
	pipelineLayouts.clear();

	for (auto it = descriptorSetLayouts.rbegin(); it != descriptorSetLayouts.rend(); it++) {
		if (*it != VK_NULL_HANDLE) {
			vkDestroyDescriptorSetLayout(device, *it, nullptr);
		}
	}
	descriptorSetLayouts.clear();

	for (auto it = descriptorPools.rbegin(); it != descriptorPools.rend(); it++) {
		if (*it != VK_NULL_HANDLE) {
			vkDestroyDescriptorPool(device, *it, nullptr);
		}
	}
	descriptorPools.clear();
}

bool DeletionBatch::empty() const
{
	return callbacks.empty() && imageViews.empty() && samplers.empty() &&
				 images.empty() && buffers.empty() && shaderModules.empty() &&
				 pipelines.empty() && pipelineLayouts.empty() &&
				 descriptorSetLayouts.empty() && descriptorPools.empty();
}

void DeletionBatch::clear()
{
	callbacks.clear();
	imageViews.clear();
	samplers.clear();
	images.clear();
	buffers.clear();
	shaderModules.clear();
	pipelines.clear();
	pipelineLayouts.clear();
	descriptorSetLayouts.clear();
	descriptorPools.clear();
}

void DeletionQueue::init(VkDevice device, VmaAllocator allocator, uint32_t frameDelay)
{
	_device = device;
	_allocator = allocator;
	_frameDelay = frameDelay;
}

void DeletionQueue::push(VkBuffer buffer, VmaAllocation allocation) {
	if (buffer == VK_NULL_HANDLE) 
		return;
	std::lock_guard<std::mutex> lock(_mutex);
	_currentBatch.buffers.emplace_back(buffer, allocation);
}

void DeletionQueue::push(VkImage image, VmaAllocation allocation) {
	if (image == VK_NULL_HANDLE) 
		return;
	std::lock_guard<std::mutex> lock(_mutex);
	_currentBatch.images.emplace_back(image, allocation);
}

void DeletionQueue::push(VkImageView imageView) {
	if (imageView == VK_NULL_HANDLE) 
		return;
	std::lock_guard<std::mutex> lock(_mutex);
	_currentBatch.imageViews.push_back(imageView);
}

void DeletionQueue::push(VkSampler sampler) {
	if (sampler == VK_NULL_HANDLE) 
		return;
	std::lock_guard<std::mutex> lock(_mutex);
	_currentBatch.samplers.push_back(sampler);
}

void DeletionQueue::push(VkShaderModule shaderModule) {
	if (shaderModule == VK_NULL_HANDLE) 
		return;
	std::lock_guard<std::mutex> lock(_mutex);
	_currentBatch.shaderModules.push_back(shaderModule);
}

void DeletionQueue::push(VkPipeline pipeline) {
	if (pipeline == VK_NULL_HANDLE) 
		return;
	std::lock_guard<std::mutex> lock(_mutex);
	_currentBatch.pipelines.push_back(pipeline);
}

void DeletionQueue::push(VkPipelineLayout layout) {
	if (layout == VK_NULL_HANDLE) 
		return;
	std::lock_guard<std::mutex> lock(_mutex);
	_currentBatch.pipelineLayouts.push_back(layout);
}

void DeletionQueue::push(VkDescriptorSetLayout layout) {
	if (layout == VK_NULL_HANDLE) 
		return;
	std::lock_guard<std::mutex> lock(_mutex);
	_currentBatch.descriptorSetLayouts.push_back(layout);
}

void DeletionQueue::push(VkDescriptorPool pool) {
	if (pool == VK_NULL_HANDLE) 
		return;
	std::lock_guard<std::mutex> lock(_mutex);
	_currentBatch.descriptorPools.push_back(pool);
}

void DeletionQueue::push_function(std::function<void()>&& callback) {
	if (!callback) 
		return;
	std::lock_guard<std::mutex> lock(_mutex);
	_currentBatch.callbacks.push_back(std::move(callback));
}

void DeletionQueue::next_frame(uint64_t frameNumber)
{
	std::lock_guard<std::mutex> lock(_mutex);
	if (!_currentBatch.empty()) {
		_pendingBatches.push_back({ frameNumber, std::move(_currentBatch) });
	}
}

void DeletionQueue::flush_retired(uint64_t completedFrameNumber)
{
	std::vector<DeletionBatch> batchesToFlush;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		while (!_pendingBatches.empty() && _pendingBatches.front().first <= completedFrameNumber) {
			batchesToFlush.push_back(std::move(_pendingBatches.front().second));
			_pendingBatches.pop_front();
		}
	}
	for (auto& batch : batchesToFlush) {
		batch.flush(_device, _allocator);
	}
}

void DeletionQueue::flush_all()
{
	std::vector<DeletionBatch> batchesToFlush;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (!_currentBatch.empty()) {
			batchesToFlush.push_back(std::move(_currentBatch));
			_currentBatch.clear();
		}
		while (!_pendingBatches.empty()) {
			batchesToFlush.push_back(std::move(_pendingBatches.front().second));
			_pendingBatches.pop_front();
		}
	}
	for (auto& batch : batchesToFlush) {
		batch.flush(_device, _allocator);
	}
}
