#pragma once

#include <graphics/utils/vk_types.h>
#include <vk_mem_alloc.h>
#include <cstdint>
#include <vector>
#include <deque>
#include <mutex>
#include <functional>
#include <span>

class VulkanCore;

struct StagedTextureUploadDesc
{
	VkImage dstImage{ VK_NULL_HANDLE };
	VkFormat format{ VK_FORMAT_R8G8B8A8_UNORM };
	VkExtent3D extent{ 1, 1, 1 };
	uint32_t mipLevels{ 1 };
	uint32_t arrayLayers{ 1 };
	VkImageAspectFlags aspectMask{ VK_IMAGE_ASPECT_COLOR_BIT };
	bool generateMips{ false };
};

class StagingUploader
{
public:
	static constexpr size_t RING_BUFFER_SIZE_BYTES = 64 * 1024 * 1024; // 64 MB

	using Callback = std::function<void()>;

	void init(VulkanCore* core, size_t ringBufferSize = RING_BUFFER_SIZE_BYTES);
	void cleanup();

	// copies cpu bytes into staging ring buffer and queues vkCmdCopyBuffer
	bool stage_buffer_upload(VkBuffer dstBuffer, VkDeviceSize dstOffset, const void* srcData, size_t byteSize, Callback onComplete = nullptr);

	// copies vertex + index slices into staging ring buffer 
	bool stage_geometry_upload(VkBuffer dstVertexBuffer, VkDeviceSize dstVertexOffset, std::span<const Vertex> vertices, VkBuffer dstIndexBuffer, VkDeviceSize dstIndexOffset, std::span<const uint32_t> indices, Callback onComplete = nullptr);

	// copies raw pixel data into staging ring buffer and queues vkCmdCopyBufferToImage and mipgen
	bool stage_texture_upload(const StagedTextureUploadDesc& desc, const void* pixelData, size_t byteSize, Callback onComplete = nullptr);

	// call at render_frame, returns true if any transfers were recorded this frame
	bool record_transfers(VkCommandBuffer cmd, uint64_t frameNumber);

	// called after vkWaitForFences to advance ring buffer tail
	void reclaim_completed_frames(uint64_t completedFrameNumber);

	size_t get_capacity_bytes() const noexcept { return _capacity; }
	size_t get_used_bytes() const noexcept;

private:
	struct StagingSlice // slice of a staging buffer that has been allocated for a copy operation
	{
		VkBuffer srcBuffer{ VK_NULL_HANDLE };
		VmaAllocation overflowAllocation{ VK_NULL_HANDLE };
		size_t offset{ 0 };
		size_t size{ 0 };
		uint8_t* mappedPtr{ nullptr };
		bool isOverflow{ false };
	};

	struct PendingBufferCopy // copy operation that has been staged but not yet submitted to the GPU
	{
		VkBuffer srcBuffer{ VK_NULL_HANDLE };
		VkBuffer dstBuffer{ VK_NULL_HANDLE };
		VkDeviceSize srcOffset{ 0 };
		VkDeviceSize dstOffset{ 0 };
		VkDeviceSize size{ 0 };
	};

	struct PendingTextureCopy // copy operation that has been staged but not yet submitted to the GPU
	{
		VkBuffer srcBuffer{ VK_NULL_HANDLE };
		VkDeviceSize srcOffset{ 0 };
		StagedTextureUploadDesc desc{};
	};

	struct InFlightBatch // batch of copy operations that have been submitted to the GPU but not yet completed
	{
		uint64_t frameNumber{ 0 };
		size_t ringEndOffset{ 0 };
		size_t bytesFromRing{ 0 };
		std::vector<std::pair<VkBuffer, VmaAllocation>> overflowBuffers;
		std::vector<Callback> completionCallbacks;
	};

	StagingSlice allocate_slice_locked(size_t size, size_t alignment = 16);

	VulkanCore* _core{ nullptr };
	VkBuffer _ringBuffer{ VK_NULL_HANDLE };
	VmaAllocation _ringAllocation{ VK_NULL_HANDLE };
	uint8_t* _ringMappedPtr{ nullptr };

	size_t _capacity{ 0 };
	size_t _writeHead{ 0 };
	size_t _readTail{ 0 };
	size_t _usedRingBytes{ 0 };
	size_t _pendingRingBytesThisFrame{ 0 };

	mutable std::mutex _mutex;
	std::vector<PendingBufferCopy> _pendingBufferCopies;
	std::vector<PendingTextureCopy> _pendingTextureCopies;
	std::vector<std::pair<VkBuffer, VmaAllocation>> _pendingOverflowBuffers;
	std::vector<Callback> _pendingCallbacks;
	std::deque<InFlightBatch> _inFlightBatches;
};