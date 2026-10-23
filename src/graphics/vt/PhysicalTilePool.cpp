#include "PhysicalTilePool.h"

#include <graphics/VulkanCore.h>
#include <graphics/utils/StagingUploader.h>
#include <graphics/utils/vk_utils.h>
#include <fmt/color.h>
#include <cassert>

void PhysicalTilePool::init(VulkanCore* core, StagingUploader* uploader, uint32_t tilesPerAxis, uint32_t layerCount,
	VkFormat format)
{
	_core = core;
	_uploader = uploader;
	_tilesPerAxis = std::clamp(tilesPerAxis, 4u, 256u);
	_totalSlots = _tilesPerAxis * _tilesPerAxis;
	_layerCount = std::max(1u, layerCount);
	_format = format;

	const uint32_t atlasPixelDim = _tilesPerAxis * VT_PHYSICAL_TILE_SIZE;
	_atlasExtent = { atlasPixelDim, atlasPixelDim, 1 };

	// initialize cpu slot table and free list
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_slots.resize(_totalSlots);
		_freeSlotStack.resize(_totalSlots);
		_pageToSlot.clear();
		_pageToSlot.reserve(_totalSlots);

		for (uint32_t i = 0; i < _totalSlots; i++)
		{
			SlotNode& node = _slots[i];
			node.slotIndex = i;
			node.tileX = static_cast<uint8_t>(i % _tilesPerAxis);
			node.tileY = static_cast<uint8_t>(i / _tilesPerAxis);
			node.inUse = false;
			node.pinned = false;
			node.lastUsedFrame = 0;
			node.residentPage = {};
			node.prevLru = VT_INVALID_PHYSICAL_SLOT;
			node.nextLru = VT_INVALID_PHYSICAL_SLOT;

			_freeSlotStack[_totalSlots - 1 - i] = i;
		}

		_lruHead = VT_INVALID_PHYSICAL_SLOT;
		_lruTail = VT_INVALID_PHYSICAL_SLOT;
		_usedSlots = 0;
		_pinnedSlots = 0;
		_cacheHits = 0;
		_cacheMisses = 0;
		_evictions = 0;
	}

	// create gpu physical atlas texture
	if (!_core)
		return;

	VkImageCreateInfo imgInfo{ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	imgInfo.imageType = VK_IMAGE_TYPE_2D;
	imgInfo.format = _format;
	imgInfo.extent = _atlasExtent;
	imgInfo.mipLevels = 1; // physical atlas has 1 mip, virtual mips stored as tiles in atlas
	imgInfo.arrayLayers = _layerCount;
	imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imgInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

	VmaAllocationCreateInfo allocInfo{};
	allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	allocInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	VmaAllocationInfo vmaInfo{};
	VK_CHECK(vmaCreateImage(_core->get_allocator(), &imgInfo, &allocInfo, &_atlasImage, &_atlasAllocation, &vmaInfo));
	_vramBytes = vmaInfo.size;

	// create image view
	VkImageViewCreateInfo viewInfo{ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	viewInfo.image = _atlasImage;
	viewInfo.viewType = (_layerCount > 1) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = _format;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.baseMipLevel = 0;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.baseArrayLayer = 0;
	viewInfo.subresourceRange.layerCount = _layerCount;
	VK_CHECK(vkCreateImageView(_core->get_device(), &viewInfo, nullptr, &_atlasImageView));

	// per layer 2d views for imgui render target view
	_layerViews.resize(_layerCount, VK_NULL_HANDLE);
	for (uint32_t layer = 0; layer < _layerCount; layer++) {
		VkImageViewCreateInfo layerViewInfo = viewInfo;
		layerViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		layerViewInfo.subresourceRange.baseArrayLayer = layer;
		layerViewInfo.subresourceRange.layerCount = 1;
		VK_CHECK(vkCreateImageView(_core->get_device(), &layerViewInfo, nullptr, &_layerViews[layer]));
	}

	// atlas linear clamp sampler
	VkSamplerCreateInfo samplerInfo{ .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.anisotropyEnable = VK_FALSE;
	samplerInfo.minLod = 0.0f;
	samplerInfo.maxLod = 0.0f;
	VK_CHECK(vkCreateSampler(_core->get_device(), &samplerInfo, nullptr, &_atlasSampler));

	// transition atlas to SHADER_READ_ONLY_OPTIMAL immediately for frame 0 binding
	_core->immediate_submit([this](VkCommandBuffer cmd) {
		vkutil::image_mem_barrier2(
			cmd,
			_atlasImage,
			_format,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			_layerCount,
			1,
			0
		);
		});

	fmt::println("PhysicalTilePool: initialized {}x{} tile atlas ({}x{} px, {} layers, {:.2f} MB VRAM)",
		_tilesPerAxis, _tilesPerAxis, _atlasExtent.width, _atlasExtent.height, _layerCount,
		static_cast<double>(_vramBytes) / (1024.0 * 1024.0));
}

void PhysicalTilePool::cleanup()
{
	std::lock_guard<std::mutex> lock(_mutex);

	if (_core)
	{
		VkDevice device = _core->get_device();
		if (_atlasSampler != VK_NULL_HANDLE)
		{
			vkDestroySampler(device, _atlasSampler, nullptr);
			_atlasSampler = VK_NULL_HANDLE;
		}

		for (VkImageView view : _layerViews)
		{
			if (view != VK_NULL_HANDLE)
			{
				vkDestroyImageView(device, view, nullptr);
			}
		}
		_layerViews.clear();

		if (_atlasImageView != VK_NULL_HANDLE)
		{
			vkDestroyImageView(device, _atlasImageView, nullptr);
			_atlasImageView = VK_NULL_HANDLE;
		}

		if (_atlasImage != VK_NULL_HANDLE)
		{
			vmaDestroyImage(_core->get_allocator(), _atlasImage, _atlasAllocation);
			_atlasImage = VK_NULL_HANDLE;
			_atlasAllocation = VK_NULL_HANDLE;
		}
	}

	_slots.clear();
	_freeSlotStack.clear();
	_pageToSlot.clear();
	_lruHead = VT_INVALID_PHYSICAL_SLOT;
	_lruTail = VT_INVALID_PHYSICAL_SLOT;
	_usedSlots = 0;
	_pinnedSlots = 0;
	_vramBytes = 0;
	_core = nullptr;
	_uploader = nullptr;
}

bool PhysicalTilePool::request_or_allocate_slot(const VirtualPageID& pageId, uint64_t currentFrame, bool pinSlot,
	PhysicalSlotCoord& outSlot, std::optional<VirtualPageID>& outEvictedPage, bool& outWasCacheHit)
{
	outEvictedPage = std::nullopt;
	outWasCacheHit = false;

	if (!pageId.is_valid())
		return false;

	std::scoped_lock<std::mutex> lock(_mutex);

	// check if page is already resident
	if (const auto existing_it = _pageToSlot.find(pageId); existing_it != _pageToSlot.end())
	{
		uint32_t slotIdx = existing_it->second;
		SlotNode& node = _slots[slotIdx];
		node.lastUsedFrame = currentFrame;

		if (pinSlot && !node.pinned)
		{
			node.pinned = true;
			_pinnedSlots++;
			lru_remove(slotIdx);
		}
		else if (!node.pinned)
		{
			lru_remove(slotIdx);
			lru_push_front(slotIdx);
		}

		outSlot = PhysicalSlotCoord{ slotIdx, node.tileX, node.tileY };
		outWasCacheHit = true;
		_cacheHits++;
		return true;
	}

	_cacheMisses++;

	uint32_t chosenSlot = VT_INVALID_PHYSICAL_SLOT;
	if (!_freeSlotStack.empty()) // allocate from free slot stack if available
	{
		chosenSlot = _freeSlotStack.back();
		_freeSlotStack.pop_back();
		_usedSlots++;
	}
	else
	{ // evict LRU unpinned slot from _lruTail
		if (_lruTail == VT_INVALID_PHYSICAL_SLOT)
		{
			fmt::println("PhysicalTilePool: out of physical tile slots, all {} slots pinned", _totalSlots);
			return false;
		}

		chosenSlot = _lruTail;
		SlotNode& node = _slots[chosenSlot];
		assert(!node.pinned && "Pinned slot cannot be in LRU eviction list");

		outEvictedPage = node.residentPage;
		_pageToSlot.erase(node.residentPage);
		lru_remove(chosenSlot);
		_evictions++;
	}


	// initialize newly assigned slot
	SlotNode& assigned = _slots[chosenSlot];
	assigned.inUse = true;
	assigned.pinned = pinSlot;
	assigned.lastUsedFrame = currentFrame;
	assigned.residentPage = pageId;

	if (pinSlot)
	{
		_pinnedSlots++;
	}
	else
	{
		lru_push_front(chosenSlot);
	}

	_pageToSlot[pageId] = chosenSlot;
	outSlot = PhysicalSlotCoord{ chosenSlot, assigned.tileX, assigned.tileY };
	return true;
}

void PhysicalTilePool::touch_slot(uint32_t slotIndex, uint64_t currentFrame)
{
	std::scoped_lock<std::mutex> lock(_mutex);

	if (slotIndex >= _slots.size())
		return;

	SlotNode& node = _slots[slotIndex];
	if (!node.inUse)
		return;

	node.lastUsedFrame = currentFrame;
	if (!node.pinned && _lruHead != slotIndex)
	{
		lru_remove(slotIndex);
		lru_push_front(slotIndex);
	}
}

void PhysicalTilePool::set_slot_pinned(uint32_t slotIndex, bool pinned)
{
	std::scoped_lock<std::mutex> lock(_mutex);

	if (slotIndex >= _slots.size())
		return;

	SlotNode& node = _slots[slotIndex];
	if (!node.inUse || node.pinned == pinned)
		return;

	node.pinned = pinned;
	if (pinned)
	{
		_pinnedSlots++;
		lru_remove(slotIndex);
	}
	else
	{
		if (_pinnedSlots > 0)
			_pinnedSlots--;
		lru_push_front(slotIndex);
	}
}

void PhysicalTilePool::free_slots_for_texture(uint32_t textureId, std::vector<VirtualPageID>& outFreedPages)
{
	std::scoped_lock<std::mutex> lock(_mutex);
	outFreedPages.clear();

	for (SlotNode& node : _slots)
	{	
		if (node.inUse && node.residentPage.textureId == textureId)
		{
			outFreedPages.push_back(node.residentPage);
			_pageToSlot.erase(node.residentPage);

			if (node.pinned)
			{
				if (_pinnedSlots > 0)
					_pinnedSlots--;
			} else
			{
				lru_remove(node.slotIndex);
			}

			node.inUse = false;
			node.pinned = false;
			node.residentPage = {};
			node.lastUsedFrame = 0;
			
			_freeSlotStack.push_back(node.slotIndex);
			if (_usedSlots > 0)
				_usedSlots--;
		}
	}
}

std::optional<PhysicalSlotCoord> PhysicalTilePool::find_resident_page(const VirtualPageID& pageId) const
{
	std::scoped_lock<std::mutex> lock(_mutex);

	auto it = _pageToSlot.find(pageId);
	if (it == _pageToSlot.end())
		return std::nullopt;
	
	const SlotNode& node = _slots[it->second];
	return PhysicalSlotCoord{ node.slotIndex, node.tileX, node.tileY };
}

bool PhysicalTilePool::stage_tile_upload(uint32_t slotIndex, uint32_t layerIndex,
                                         std::span<const uint8_t> tilePixelsWithBorder)
{
	if (!_uploader || _atlasImage == VK_NULL_HANDLE)
		return false;

	if (slotIndex >= _totalSlots || layerIndex >= _layerCount)
		return false;

	constexpr size_t expectedBytes = static_cast<size_t>(VT_PHYSICAL_TILE_SIZE) * VT_PHYSICAL_TILE_SIZE * 4u;
	if (tilePixelsWithBorder.size_bytes() < expectedBytes)
	{
		fmt::println("PhysicalTilePool: tile upload buffer too small: {} expected {}", tilePixelsWithBorder.size_bytes(), expectedBytes);
		return false;
	}

	uint32_t tileX = slotIndex % _tilesPerAxis;
	uint32_t tileY = slotIndex / _tilesPerAxis;

	StagedTextureRegionDesc desc{};
	desc.dstImage = _atlasImage;
	desc.format = _format;
	desc.offset = { static_cast<int32_t>(tileX * VT_PHYSICAL_TILE_SIZE), static_cast<int32_t>(tileY * VT_PHYSICAL_TILE_SIZE), 0 };
	desc.extent = { VT_PHYSICAL_TILE_SIZE, VT_PHYSICAL_TILE_SIZE, 1 };
	desc.mipLevel = 0;
	desc.totalMipLevels = 1;
	desc.baseArrayLayer = layerIndex;
	desc.layerCount = 1;
	desc.totalArrayLayers = _layerCount;
	desc.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	desc.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	desc.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	return _uploader->stage_texture_region_upload(desc, tilePixelsWithBorder.data(), expectedBytes);
}

void PhysicalTilePool::lru_remove(uint32_t slotIndex)
{
	SlotNode& node = _slots[slotIndex];

	if (node.prevLru != VT_INVALID_PHYSICAL_SLOT)
		_slots[node.prevLru].nextLru = node.nextLru;
	else if (_lruHead == slotIndex)
		_lruHead = node.nextLru;

	if (node.nextLru != VT_INVALID_PHYSICAL_SLOT)
		_slots[node.nextLru].prevLru = node.prevLru;
	else if (_lruTail == slotIndex)
		_lruTail = node.prevLru;

	node.prevLru = VT_INVALID_PHYSICAL_SLOT;
	node.nextLru = VT_INVALID_PHYSICAL_SLOT;
}

void PhysicalTilePool::lru_push_front(uint32_t slotIndex)
{
	SlotNode& node = _slots[slotIndex];
	node.prevLru = VT_INVALID_PHYSICAL_SLOT;
	node.nextLru = _lruHead;

	if (_lruHead != VT_INVALID_PHYSICAL_SLOT)
		_slots[_lruHead].prevLru = slotIndex;
	_lruHead = slotIndex;

	if (_lruTail == VT_INVALID_PHYSICAL_SLOT)
		_lruTail = slotIndex;
}
