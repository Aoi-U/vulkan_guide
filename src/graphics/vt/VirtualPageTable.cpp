#include "VirtualPageTable.h"

#include <graphics/VulkanCore.h>
#include <graphics/utils/StagingUploader.h>
#include <graphics/utils/vk_utils.h>
#include <fmt/core.h>
#include <cmath>
#include <algorithm>
#include <cassert>

void VirtualPageTable::init(VulkanCore* core, StagingUploader* uploader, uint32_t pageTableSizeMip0,
	uint32_t maxMipLevels)
{
	_core = core;
	_uploader = uploader;
	_sizeMip0 = std::max(64u, pageTableSizeMip0);
	uint32_t maxPossibleMips = static_cast<uint32_t>(std::floor(std::log2(_sizeMip0))) + 1;
	_mipCount = std::min(maxMipLevels, maxPossibleMips);

	_macroBlockSize = 16; // 16 tiles = 2048 texels per unit
	_macroGridWidth = std::max(1u, _sizeMip0 / _macroBlockSize);

	{
		std::scoped_lock<std::mutex> lock(_mutex);
		_virtualSpaceAllocator = std::make_unique<OffsetAllocator::Allocator>(_sizeMip0 * _sizeMip0);
		_allocatedTilesMip0 = 0;

		_mips.resize(_mipCount);
		for (uint32_t m = 0; m < _mipCount; m++)
		{
			uint32_t dim = std::max(1u, _sizeMip0 >> m);
			_mips[m].width = dim;
			_mips[m].height = dim;
			_mips[m].entries.assign(dim * dim, PageTableEntry::empty());
			_mips[m].exactEntries.assign(dim * dim, PageTableEntry::empty());
			_mips[m].dirtyRect.reset();
		}
	}

	if (!_core)
		return;

	// create gpu page table texture 
	VkImageCreateInfo imgInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = nullptr,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R32_UINT,
		.extent = { _sizeMip0, _sizeMip0, 1},
		.mipLevels = _mipCount,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
	};

	VmaAllocationCreateInfo allocInfo{};
	allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	allocInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	VmaAllocationInfo vmaInfo{};
	VK_CHECK(vmaCreateImage(_core->get_allocator(), &imgInfo, &allocInfo, &_pageTableImage, &_pageTableAlloc, &vmaInfo));
	_vramBytes = vmaInfo.size;

	VkImageViewCreateInfo viewInfo{};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = _pageTableImage;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = VK_FORMAT_R32_UINT;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.baseMipLevel = 0;
	viewInfo.subresourceRange.levelCount = _mipCount;
	viewInfo.subresourceRange.baseArrayLayer = 0;
	viewInfo.subresourceRange.layerCount = 1;
	VK_CHECK(vkCreateImageView(_core->get_device(), &viewInfo, nullptr, &_pageTableView));

	// nearest sampler for integer page table sampling
	VkSamplerCreateInfo samplerInfo{
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.pNext = nullptr,
		.magFilter = VK_FILTER_NEAREST,
		.minFilter = VK_FILTER_NEAREST,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.minLod = 0.0f,
		.maxLod = static_cast<float>(_mipCount)
	};
	VK_CHECK(vkCreateSampler(_core->get_device(), &samplerInfo, nullptr, &_nearestSampler));

	_core->immediate_submit([this](VkCommandBuffer cmd)
		{
			// transition to transfer dst layout
			vkutil::image_mem_barrier2(
				cmd,
				_pageTableImage,
				VK_FORMAT_R32_UINT,
				VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				1,
				_mipCount,
				0
			);

			// clear page table to 0
			VkClearColorValue clearVal{};
			clearVal.uint32[0] = 0;
			VkImageSubresourceRange range{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = _mipCount,
				.baseArrayLayer = 0,
				.layerCount = 1
			};
			vkCmdClearColorImage(cmd, _pageTableImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearVal, 1, &range);

			// transition to shader read layout
			vkutil::image_mem_barrier2(
				cmd,
				_pageTableImage,
				VK_FORMAT_R32_UINT,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				1,
				_mipCount,
				0
			);
		});

	fmt::println("VirtualPageTable: initialized {}x{} page table with {} mip levels, {:.2f} KB VRAM", _sizeMip0, _sizeMip0, _mipCount, static_cast<double>(_vramBytes) / 1024.0f);
}

void VirtualPageTable::cleanup()
{
	std::scoped_lock<std::mutex> lock(_mutex);

	if (_core)
	{
		VkDevice dev = _core->get_device();
		if (_nearestSampler != VK_NULL_HANDLE)
		{
			vkDestroySampler(dev, _nearestSampler, nullptr);
			_nearestSampler = VK_NULL_HANDLE;
		}

		if (_pageTableView != VK_NULL_HANDLE)
		{
			vkDestroyImageView(dev, _pageTableView, nullptr);
			_pageTableView = VK_NULL_HANDLE;
		}

		if (_pageTableImage != VK_NULL_HANDLE)
		{
			vmaDestroyImage(_core->get_allocator(), _pageTableImage, _pageTableAlloc);
			_pageTableImage = VK_NULL_HANDLE;
			_pageTableAlloc = VK_NULL_HANDLE;
		}
	}

	_mips.clear();
	_virtualSpaceAllocator = nullptr;
	_allocatedTilesMip0 = 0;
	_vramBytes = 0;
	_core = nullptr;
	_uploader = nullptr;
}

PageTableAllocation VirtualPageTable::allocate_virtual_range(uint16_t textureId, uint32_t virtualWidth,
	uint32_t virtualHeight, uint32_t mipLevels)
{
	std::scoped_lock<std::mutex> lock(_mutex);

	uint32_t tilesX = std::max(1u, (virtualWidth + VT_LOGICAL_TILE_SIZE - 1u) / VT_LOGICAL_TILE_SIZE);
	uint32_t tilesY = std::max(1u, (virtualHeight + VT_LOGICAL_TILE_SIZE - 1u) / VT_LOGICAL_TILE_SIZE);

	// round up to next power of two for mip shift alignment without overlap
	uint32_t span = std::max(tilesX, tilesY);
	uint32_t pow2Span = 1u;
	while (pow2Span < span)
		pow2Span <<= 1u;

	// allocate aligned macro blocks along page table
	uint32_t blockSpan = std::max(1u, (pow2Span + _macroBlockSize - 1u) / _macroBlockSize);
	uint32_t neededUnits = blockSpan * blockSpan;

	OffsetAllocator::Allocation rawAlloc = _virtualSpaceAllocator->allocate(neededUnits);
	if (rawAlloc.offset == OffsetAllocator::Allocation::NO_SPACE)
	{
		fmt::println("VirtualPageTable: failed to suballocate {}x{} tiles for texture {}", tilesX, tilesY, textureId);
		return PageTableAllocation{};
	}

	// map 1d offset to 2d macro block coordinates
	uint32_t macroIndex = rawAlloc.offset;
	uint32_t tileOffsetX = (macroIndex % _macroGridWidth) * _macroBlockSize;
	uint32_t tileOffsetY = (macroIndex / _macroGridWidth) * _macroBlockSize;

	if (tileOffsetX + pow2Span > _sizeMip0 || tileOffsetY + pow2Span > _sizeMip0)
	{
		_virtualSpaceAllocator->free(rawAlloc);
		fmt::println("VirtualPageTable: virtual address range exceeded {}x{} page table bounds", _sizeMip0, _sizeMip0);
		return PageTableAllocation{};
	}

	_allocatedTilesMip0 += (tilesX * tilesY);

	PageTableAllocation result{};
	result.textureId = textureId;
	result.offsetX = tileOffsetX;
	result.offsetY = tileOffsetY;
	result.tilesX = tilesX;
	result.tilesY = tilesY;
	result.mipLevels = std::min(mipLevels, _mipCount);
	result.regionAlloc = rawAlloc;
	return result;
}

void VirtualPageTable::free_virtual_range(const PageTableAllocation& alloc)
{
	if (!alloc.is_valid())
		return;

	std::scoped_lock<std::mutex> lock(_mutex);

	// clear all mip entries for this virtual texture
	for (uint32_t mip = 0; mip < alloc.mipLevels && mip < _mipCount; mip++)
	{
		uint32_t mipTilesX = std::max(1u, alloc.tilesX >> mip);
		uint32_t mipTilesY = std::max(1u, alloc.tilesY >> mip);
		uint32_t baseX = alloc.offsetX >> mip;
		uint32_t baseY = alloc.offsetY >> mip;

		for (uint32_t tileY = 0; tileY < mipTilesY; tileY++)
		{
			for (uint32_t tileX = 0; tileX < mipTilesX; tileX++)
			{
				uint32_t gx = baseX + tileX;
				uint32_t gy = baseY + tileY;
				if (gx < _mips[mip].width && gy < _mips[mip].height)
				{
					size_t idx = static_cast<size_t>(gy) * _mips[mip].width + gx;
					_mips[mip].exactEntries[idx] = PageTableEntry::empty();
					write_entry(static_cast<uint8_t>(mip), gx, gy, PageTableEntry::empty());
				}
			}
		}
	}

	if (alloc.regionAlloc.offset != OffsetAllocator::Allocation::NO_SPACE && _virtualSpaceAllocator)
	{
		_virtualSpaceAllocator->free(alloc.regionAlloc);
	}

	uint32_t freedTiles = alloc.tilesX * alloc.tilesY;
	_allocatedTilesMip0 = (_allocatedTilesMip0 >= freedTiles) ? (_allocatedTilesMip0 - freedTiles) : 0;
}

void VirtualPageTable::map_page(const PageTableAllocation& alloc, uint16_t localTileX, uint16_t localTileY,
	uint8_t mipLevel, uint8_t physTileX, uint8_t physTileY)
{
	if (!alloc.is_valid() || mipLevel >= alloc.mipLevels || mipLevel >= _mipCount)
		return;

	std::scoped_lock<std::mutex> lock(_mutex);

	uint32_t gx = (alloc.offsetX >> mipLevel) + localTileX;
	uint32_t gy = (alloc.offsetY >> mipLevel) + localTileY;
	MipLevelStorage& storage = _mips[mipLevel];
	if (gx >= storage.width || gy >= storage.height)
		return;

	PageTableEntry exactEntry = PageTableEntry::pack(physTileX, physTileY, mipLevel, true);
	storage.exactEntries[static_cast<size_t>(gy) * storage.width + gx] = exactEntry;
	write_entry(mipLevel, gx, gy, exactEntry);

	// propagate fallback entry down to lower mips for this virtual page
	if (mipLevel > 0)
	{
		propagate_fallback_down(alloc, localTileX, localTileY, mipLevel, exactEntry.as_fallback());
	}
}

void VirtualPageTable::unmap_page(const PageTableAllocation& alloc, uint16_t localTileX, uint16_t localTileY,
	uint8_t mipLevel)
{
	if (!alloc.is_valid() || mipLevel >= alloc.mipLevels || mipLevel >= _mipCount)
		return;

	std::scoped_lock<std::mutex> lock(_mutex);

	uint32_t gx = (alloc.offsetX >> mipLevel) + localTileX;
	uint32_t gy = (alloc.offsetY >> mipLevel) + localTileY;
	MipLevelStorage& storage = _mips[mipLevel];
	if (gx >= storage.width || gy >= storage.height)
		return;

	// clear exact entry at this mip
	storage.exactEntries[static_cast<size_t>(gy) * storage.width + gx] = PageTableEntry::empty();

	// search upwards for nearest resident parent mip fallback
	PageTableEntry replacement = PageTableEntry::empty();
	for (uint8_t parentMip = mipLevel + 1; parentMip < alloc.mipLevels && parentMip < _mipCount; parentMip++)
	{
		uint32_t shift = parentMip - mipLevel;
		uint32_t px = (alloc.offsetX >> parentMip) + (localTileX >> shift);
		uint32_t py = (alloc.offsetY >> parentMip) + (localTileY >> shift);
		const MipLevelStorage& parentStorage = _mips[parentMip];

		if (px < parentStorage.width && py < parentStorage.height)
		{
			PageTableEntry parentExact = parentStorage.exactEntries[static_cast<size_t>(py) * parentStorage.width + px];
			if (parentExact.is_resident())
			{
				replacement = parentExact.as_fallback();
				break;
			}
		}
	}

	write_entry(mipLevel, gx, gy, replacement);

	// propagate parent fallback down to lower mips for this virtual page
	if (mipLevel > 0)
	{
		propagate_fallback_down(alloc, localTileX, localTileY, mipLevel, replacement);
	}
}

uint32_t VirtualPageTable::flush_dirty_pages()
{
	std::scoped_lock<std::mutex> lock(_mutex);
	uint32_t flushedMipCount = 0;

	for (uint32_t mip = 0; mip < _mipCount; mip++)
	{
		MipLevelStorage& storage = _mips[mip];
		if (!storage.dirtyRect.is_dirty())
			continue;

		const uint32_t rectX = storage.dirtyRect.minX;
		const uint32_t rectY = storage.dirtyRect.minY;
		const uint32_t rectW = storage.dirtyRect.maxX - rectX + 1u;
		const uint32_t rectH = storage.dirtyRect.maxY - rectY + 1u;

		if (_uploader && _pageTableImage != VK_NULL_HANDLE)
		{
			// pack dirty subrectangle into contiguous buffer for StagingUploader
			std::vector<uint32_t> contiguousRect(static_cast<size_t>(rectW) * rectH);
			for (uint32_t row = 0; row < rectH; row++)
			{
				const PageTableEntry* srcRow = &storage.entries[static_cast<size_t>(rectY + row) * storage.width + rectX];
				memcpy(&contiguousRect[static_cast<size_t>(row) * rectW], srcRow, rectW * sizeof(uint32_t));
			}

			StagedTextureRegionDesc desc{};
			desc.dstImage = _pageTableImage;
			desc.format = VK_FORMAT_R32_UINT;
			desc.offset = { static_cast<int32_t>(rectX), static_cast<int32_t>(rectY), 0 };
			desc.extent = { rectW, rectH, 1 };
			desc.mipLevel = mip;
			desc.totalMipLevels = _mipCount;
			desc.baseArrayLayer = 0;
			desc.layerCount = 1;
			desc.totalArrayLayers = 1;
			desc.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			desc.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			desc.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			_uploader->stage_texture_region_upload(desc, contiguousRect.data(), contiguousRect.size() * sizeof(uint32_t));
		}

		storage.dirtyRect.reset();
		flushedMipCount++;
	}

	return flushedMipCount;
}

PageTableEntry VirtualPageTable::get_cpu_entry(uint32_t globalTileX, uint32_t globalTileY, uint8_t mipLevel) const
{
	std::scoped_lock<std::mutex> lock(_mutex);
	if (mipLevel >= _mipCount)
		return PageTableEntry::empty();

	const MipLevelStorage& storage = _mips[mipLevel];
	if (globalTileX >= storage.width || globalTileY >= storage.height)
		return PageTableEntry::empty();

	return storage.entries[static_cast<size_t>(globalTileY) * storage.width + globalTileX];
}

void VirtualPageTable::write_entry(uint8_t mip, uint32_t x, uint32_t y, PageTableEntry entry)
{
	MipLevelStorage& storage = _mips[mip];
	size_t idx = static_cast<size_t>(y) * storage.width + x;
	if (storage.entries[idx].raw != entry.raw)
	{
		storage.entries[idx] = entry;
		storage.dirtyRect.expand(x, y);
	}
}

void VirtualPageTable::propagate_fallback_down(const PageTableAllocation& alloc, uint16_t localX, uint16_t localY,
	uint8_t sourceMip, PageTableEntry fallbackEntry)
{
	// recursively walk quadtree children from (sourceMip - 1) down to mip0
	// stop on any subtree branch that already has its own resident tile
	for (uint8_t childMip = sourceMip; childMip-- > 0; )
	{
		uint32_t shift = sourceMip - childMip;
		uint32_t childStartX = static_cast<uint32_t>(localX) << shift;
		uint32_t childStartY = static_cast<uint32_t>(localY) << shift;
		uint32_t span = 1u << shift;

		uint32_t mipTilesX = std::max(1u, alloc.tilesX >> childMip);
		uint32_t mipTilesY = std::max(1u, alloc.tilesY >> childMip);
		uint32_t baseX = alloc.offsetX >> childMip;
		uint32_t baseY = alloc.offsetY >> childMip;

		MipLevelStorage& childStorage = _mips[childMip];

		for (uint32_t dy = 0; dy < span; dy++)
		{
			uint32_t cy = childStartY + dy;
			if (cy >= mipTilesY)
				continue;

			for (uint32_t dx = 0; dx < span; dx++)
			{
				uint32_t cx = childStartX + dx;
				if (cx >= mipTilesX)
					continue;

				// check if this child or any intermediate ancestor between childMIp and sourceMip has an exact resident page
				bool coveredByFinerAncestor = false;
				for (uint8_t checkMip = childMip; checkMip < sourceMip; checkMip++)
				{
					uint32_t ancShift = checkMip - childMip;
					uint32_t ax = (alloc.offsetX >> checkMip) + (cx >> ancShift);
					uint32_t ay = (alloc.offsetY >> checkMip) + (cy >> ancShift);
					if (_mips[checkMip].exactEntries[static_cast<size_t>(ay) * _mips[checkMip].width + ax].is_resident())
					{
						coveredByFinerAncestor = true;
						break;
					}
				}

				if (!coveredByFinerAncestor)
					write_entry(childMip, baseX + cx, baseY + cy, fallbackEntry);
			}
		}
	}
}
