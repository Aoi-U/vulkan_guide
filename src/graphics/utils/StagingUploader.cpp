#include "StagingUploader.h"

#include <graphics/VulkanCore.h>
#include <graphics/utils/vk_utils.h>
#include <fmt/core.h>
#include <cstring>
#include <algorithm>

void StagingUploader::init(VulkanCore* core, size_t ringBufferSize)
{
	_core = core;
	_capacity = ringBufferSize;
	_writeHead = 0;
	_readTail = 0;
	_usedRingBytes = 0;
	_pendingRingBytesThisFrame = 0;

	VkBufferCreateInfo bufferInfo{
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = _capacity,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT
	};

	VmaAllocationCreateInfo allocInfo{
		.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
		.usage = VMA_MEMORY_USAGE_CPU_TO_GPU
	};

	VmaAllocationInfo outInfo{};
	VK_CHECK(vmaCreateBuffer(_core->get_allocator(), &bufferInfo, &allocInfo, &_ringBuffer, &_ringAllocation, &outInfo));
	_ringMappedPtr = static_cast<uint8_t*>(outInfo.pMappedData);
}

void StagingUploader::cleanup()
{
	std::lock_guard<std::mutex> lock(_mutex);

	for (auto& batch : _inFlightBatches) {
		for (auto& [buf, alloc] : batch.overflowBuffers) {
			vmaDestroyBuffer(_core->get_allocator(), buf, alloc);
		}
	}
	_inFlightBatches.clear();

	for (auto& [buf, alloc] : _pendingOverflowBuffers) {
		vmaDestroyBuffer(_core->get_allocator(), buf, alloc);
	}
	_pendingOverflowBuffers.clear();
	_pendingBufferCopies.clear();
	_pendingTextureCopies.clear();
	_pendingCallbacks.clear();

	if (_ringBuffer != VK_NULL_HANDLE) {
		vmaDestroyBuffer(_core->get_allocator(), _ringBuffer, _ringAllocation);
		_ringBuffer = VK_NULL_HANDLE;
		_ringAllocation = VK_NULL_HANDLE;
		_ringMappedPtr = nullptr;
	}
}

bool StagingUploader::stage_buffer_upload(VkBuffer dstBuffer, VkDeviceSize dstOffset, const void* srcData, size_t byteSize, Callback onComplete)
{
	if (dstBuffer == VK_NULL_HANDLE || !srcData || byteSize == 0)
		return false;

	std::lock_guard<std::mutex> lock(_mutex);

	// allocate a slice of staging buffer 
	StagingSlice slice = allocate_slice_locked(byteSize, 16);
	if (!slice.mappedPtr)
		return false;

	// copy data into staging buffer slice
	memcpy(slice.mappedPtr, srcData, byteSize);

	// queue buffer copy operation
	_pendingBufferCopies.push_back(PendingBufferCopy{
		.srcBuffer = slice.srcBuffer,
		.dstBuffer = dstBuffer,
		.srcOffset = slice.offset,
		.dstOffset = dstOffset,
		.size = byteSize
		});

	if (onComplete) {
		_pendingCallbacks.push_back(std::move(onComplete));
	}

	return true;
}

bool StagingUploader::stage_geometry_upload(VkBuffer dstVertexBuffer, VkDeviceSize dstVertexOffset, std::span<const Vertex> vertices, VkBuffer dstIndexBuffer, VkDeviceSize dstIndexOffset, std::span<const uint32_t> indices, Callback onComplete)
{
	if (vertices.empty() || indices.empty())
		return false;

	const size_t vertBytes = vertices.size_bytes();
	const size_t idxBytes = indices.size_bytes();

	std::lock_guard<std::mutex> lock(_mutex);

	// allocate a slice of staging buffer 
	StagingSlice slice = allocate_slice_locked(vertBytes + idxBytes, 16);
	if (!slice.mappedPtr)
		return false;

	// copy vertex and index data into staging buffer slice
	memcpy(slice.mappedPtr, vertices.data(), vertBytes);
	memcpy(slice.mappedPtr + vertBytes, indices.data(), idxBytes);

	// queue buffer copy operations for vertex and index data
	_pendingBufferCopies.push_back(PendingBufferCopy{
		.srcBuffer = slice.srcBuffer,
		.dstBuffer = dstVertexBuffer,
		.srcOffset = slice.offset,
		.dstOffset = dstVertexOffset,
		.size = vertBytes
		});
	_pendingBufferCopies.push_back(PendingBufferCopy{
		.srcBuffer = slice.srcBuffer,
		.dstBuffer = dstIndexBuffer,
		.srcOffset = slice.offset + vertBytes,
		.dstOffset = dstIndexOffset,
		.size = idxBytes
		});

	if (onComplete) {
		_pendingCallbacks.push_back(std::move(onComplete));
	}
	
	return true;
}

bool StagingUploader::stage_texture_upload(const StagedTextureUploadDesc& desc, const void* pixelData, size_t byteSize, Callback onComplete)
{
	if (desc.dstImage == VK_NULL_HANDLE || !pixelData || byteSize == 0)
		return false;

	std::lock_guard<std::mutex> lock(_mutex);

	// allocate a slice of staging buffer
	StagingSlice slice = allocate_slice_locked(byteSize, 16);
	if (!slice.mappedPtr)
		return false;

	// copy pixel data into staging buffer slice
	memcpy(slice.mappedPtr, pixelData, byteSize);

	// queue texture copy operation
	_pendingTextureCopies.push_back(PendingTextureCopy{
		.srcBuffer = slice.srcBuffer,
		.srcOffset = slice.offset,
		.desc = desc
		});

	if (onComplete) {
		_pendingCallbacks.push_back(std::move(onComplete));
	}

	return true;
}

bool StagingUploader::record_transfers(VkCommandBuffer cmd, uint64_t frameNumber)
{
	std::vector<PendingBufferCopy> bufCopies;
	std::vector<PendingTextureCopy> texCopies;
	std::vector<std::pair<VkBuffer, VmaAllocation>> overflows;
	std::vector<std::function<void()>> callbacks;
	size_t ringBytesThisBatch = 0;
	size_t ringHeadSnapshot = 0;

	// swap out pending copies and overflow buffers to local vectors for processing
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (_pendingBufferCopies.empty() || _pendingTextureCopies.empty()) {
			return false;
		}
		bufCopies.swap(_pendingBufferCopies);
		texCopies.swap(_pendingTextureCopies);
		overflows.swap(_pendingOverflowBuffers);
		callbacks.swap(_pendingCallbacks);

		ringBytesThisBatch = _pendingRingBytesThisFrame;
		_pendingRingBytesThisFrame = 0;
		ringHeadSnapshot = _writeHead;
	}

	// record all pending buffer copies
	for (const PendingBufferCopy& copy : bufCopies) {
		VkBufferCopy region{
			.srcOffset = copy.srcOffset,
			.dstOffset = copy.dstOffset,
			.size = copy.size
		};
		vkCmdCopyBuffer(cmd, copy.srcBuffer, copy.dstBuffer, 1, &region);
	}
	
	// record all pending texture uploads and mipmap gen
	for (const PendingTextureCopy& tex : texCopies) {
		vkutil::image_mem_barrier2(
			cmd,
			tex.desc.dstImage,
			tex.desc.format,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			tex.desc.arrayLayers,
			tex.desc.mipLevels,
			0
		); 

		VkBufferImageCopy region{};
		region.bufferOffset = tex.srcOffset;
		region.imageSubresource.aspectMask = tex.desc.aspectMask;
		region.imageSubresource.mipLevel = 0;
		region.imageSubresource.baseArrayLayer = 0;
		region.imageSubresource.layerCount = tex.desc.arrayLayers;
		region.imageExtent = tex.desc.extent;

		vkCmdCopyBufferToImage(cmd, tex.srcBuffer, tex.desc.dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		if (tex.desc.generateMips && tex.desc.mipLevels > 1) {
			vkutil::generate_mipmaps(cmd, tex.desc.dstImage, { tex.desc.extent.width, tex.desc.extent.height });
		}
		else {
			vkutil::image_mem_barrier2(
				cmd,
				tex.desc.dstImage,
				tex.desc.format,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				tex.desc.arrayLayers,
				tex.desc.mipLevels,
				0
			);
		}
	}

	// emit memory barrier 
	if (!bufCopies.empty()) {
		VkMemoryBarrier2 transferBarrier{
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
											VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
											VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
											VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
			.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT
		};

		VkDependencyInfo depInfo{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.memoryBarrierCount = 1,
			.pMemoryBarriers = &transferBarrier
		};
		vkCmdPipelineBarrier2(cmd, &depInfo);
	}

	// track inflight batch for reclaiming ring buffer space when fence is signaled
	{
		std::lock_guard<std::mutex> lock(_mutex);

		_inFlightBatches.push_back(InFlightBatch{
			.frameNumber = frameNumber,
			.ringEndOffset = ringHeadSnapshot,
			.bytesFromRing = ringBytesThisBatch,
			.overflowBuffers = std::move(overflows),
			.completionCallbacks = std::move(callbacks)
			});
	}

	return true;
}

void StagingUploader::reclaim_completed_frames(uint64_t completedFrameNumber)
{
	std::vector<Callback> readyCallbacks;

	{
		std::lock_guard<std::mutex> lock(_mutex);
		
		while (!_inFlightBatches.empty() && _inFlightBatches.front().frameNumber <= completedFrameNumber) {
			InFlightBatch& batch = _inFlightBatches.front();

			// advance ring buffer tail and reclaim used bytes
			_readTail = batch.ringEndOffset;
			if (_usedRingBytes >= batch.bytesFromRing) {
				_usedRingBytes -= batch.bytesFromRing;
			}
			else {
				_usedRingBytes = 0;
			}

			// destroy overflow buffers that were allocated for this batch
			for (auto& [buf, alloc] : batch.overflowBuffers) {
				vmaDestroyBuffer(_core->get_allocator(), buf, alloc);
			}

			// move completion callbacks to readyCallbacks vector for invocation 
			for (Callback& cb : batch.completionCallbacks) {
				readyCallbacks.push_back(std::move(cb));
			}

			_inFlightBatches.pop_front();
		}
	}

	for (Callback& cb : readyCallbacks) {
		if (cb)
			cb();
	}
}

size_t StagingUploader::get_used_bytes() const noexcept
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _usedRingBytes;
}

StagingUploader::StagingSlice StagingUploader::allocate_slice_locked(size_t size, size_t alignment)
{
	size_t alignedHead = (_writeHead + alignment - 1) & ~(alignment - 1);
	size_t padding = alignedHead - _writeHead;
	
	// if the slice wraps past end of 64mb buffer, check if we can wrap to 0
	if (alignedHead + size > _capacity) {
		size_t wrapWaste = _capacity - _writeHead;
		if (_usedRingBytes + wrapWaste + size <= _capacity) { 
			_usedRingBytes += wrapWaste;
			_pendingRingBytesThisFrame += wrapWaste;
			_writeHead = 0;
			alignedHead = 0;
			padding = 0;
		}
	}

	// check if there is enough space in ring buffer for requested slice
	if (alignedHead + size <= _capacity && (_usedRingBytes + padding + size <= _capacity)) {
		// allocate from ring buffer
		StagingSlice slice{};
		slice.srcBuffer = _ringBuffer;
		slice.offset = alignedHead;
		slice.size = size;
		slice.mappedPtr = _ringMappedPtr + alignedHead;
		slice.isOverflow = false;

		_writeHead = alignedHead + size;
		_usedRingBytes += (padding + size);
		_pendingRingBytesThisFrame += (padding + size);
		return slice;
	}

	// ring buffer temporarily full this frame, allocate transient overflow buffer
	VkBufferCreateInfo bufInfo{
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT
	};
	VmaAllocationCreateInfo allocInfo{
		.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
		.usage = VMA_MEMORY_USAGE_CPU_TO_GPU
	};
	
	// allocate overflow buffer
	VkBuffer overflowBuf = VK_NULL_HANDLE;
	VmaAllocation overflowAlloc = VK_NULL_HANDLE;
	VmaAllocationInfo outInfo{};
	if (vmaCreateBuffer(_core->get_allocator(), &bufInfo, &allocInfo, &overflowBuf, &overflowAlloc, &outInfo) != VK_SUCCESS) {
		fmt::println("StagingUploader: failed to allocate overflow staging buffer of size {}", size);
		return StagingSlice{};
	}
	_pendingOverflowBuffers.emplace_back(overflowBuf, overflowAlloc);

	// return the overflow slice
	StagingSlice slice{};
	slice.srcBuffer = overflowBuf;
	slice.overflowAllocation = overflowAlloc;
	slice.offset = 0;
	slice.size = size;
	slice.mappedPtr = static_cast<uint8_t*>(outInfo.pMappedData);
	slice.isOverflow = true;
	
	return slice;
}
