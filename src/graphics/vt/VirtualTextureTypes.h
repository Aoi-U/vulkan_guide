#pragma once

#include <graphics/utils/vk_types.h>
#include <core/UUID.h>
#include <cstdint>
#include <functional>

#include "offsetAllocator.hpp"

// texel dimensions of a single virtual tile before border padding
static constexpr uint32_t VT_LOGICAL_TILE_SIZE = 128;

// texel border size of a single virtual tile, used to avoid bilinear and anisotropic filtering artifacts
static constexpr uint32_t VT_TILE_BORDER = 4;

// actual texel dimensions of a single virtual tile
static constexpr uint32_t VT_PHYSICAL_TILE_SIZE = VT_LOGICAL_TILE_SIZE + (2 * VT_TILE_BORDER);

// max mip levels per virtual texture
static constexpr uint32_t VT_MAX_MIP_LEVELS = 12;

// default physical atlas grid dimension in tiles
static constexpr uint32_t VT_DEFAULT_ATLAS_TILES_PER_AXIS = 32;

// default page table mip 0 dimensions in tiles
static constexpr uint32_t VT_DEFAULT_PAGE_TABLE_SIZE = 1024;

static constexpr uint16_t VT_INVALID_TEXTURE_ID = UINT16_MAX;
static constexpr uint32_t VT_INVALID_PHYSICAL_SLOT = UINT32_MAX;

// physical atlas texture layers for standard pbr materials
enum class VTPhysicalLayer : uint32_t
{
	Albedo = 0,
	Normal = 1,
	MetalRoughOccl = 2,
	Count = 3
};

// unique identifier for a single virtual tile at a specific mip
struct VirtualPageID
{
	uint16_t textureId{ VT_INVALID_TEXTURE_ID };
	uint16_t tileX{ 0 };
	uint16_t tileY{ 0 };
	uint8_t mipLevel{ 0 };
	uint8_t padding{ 0 };

	constexpr bool is_valid() const noexcept
	{
		return textureId != VT_INVALID_TEXTURE_ID;
	}

	constexpr uint64_t to_u64() const noexcept
	{
		return (static_cast<uint64_t>(textureId) << 48ULL) | (static_cast<uint64_t>(tileX) << 32ULL) | (static_cast<uint64_t>(tileY) << 16ULL) | mipLevel;
	}

	static constexpr VirtualPageID from_u64(uint64_t packed) noexcept
	{
		VirtualPageID id{};
		id.textureId = static_cast<uint16_t>((packed >> 48ULL) & 0xFFFFULL);
		id.tileX = static_cast<uint16_t>((packed >> 32ULL) & 0xFFFFULL);
		id.tileY = static_cast<uint16_t>((packed >> 16ULL) & 0xFFFFULL);
		id.mipLevel = static_cast<uint8_t>(packed & 0xFFULL);
		return id;
	}

	friend constexpr bool operator==(const VirtualPageID& a, const VirtualPageID& b) noexcept
	{
		return a.to_u64() == b.to_u64();
	}

	friend constexpr bool operator !=(const VirtualPageID& a, const VirtualPageID& b) noexcept
	{
		return a.to_u64() != b.to_u64();
	}
};
static_assert(sizeof(VirtualPageID) == 8, "VirtualPageID must be 8 bytes");

namespace std
{
	template<>
	struct hash<VirtualPageID>
	{
		size_t operator()(const VirtualPageID& id) const noexcept
		{
			return std::hash<uint64_t>()(id.to_u64());
		}
	};
}

// single entry in table entry
// 32 bit:
//	0: isResident (1 = mapped to valid physical tile, 0 = unmapped)
//	1-8: physicalTileX (0-255 tile column in physical atlas)
//	9-16: physicalTileY (0-255 tile row in physical atlas
//	17-20: actualMipLevel (0-15 actual mip level stored in physical slot
//	21-31: reserved/flags
struct PageTableEntry
{
	uint32_t raw{ 0 };

	static constexpr uint32_t RESIDENT_BIT = 1u << 0;
	static constexpr uint32_t EXACT_MIP_BIT = 1u << 21;

	static constexpr PageTableEntry empty() noexcept
	{
		return PageTableEntry{ 0 };
	}

	static constexpr PageTableEntry pack(uint8_t physTileX, uint8_t physTileY, uint8_t actualMip, bool isExactMip = true) noexcept
	{
		uint32_t val = RESIDENT_BIT;
		val |= (static_cast<uint32_t>(physTileX) & 0xFFu) << 1u;
		val |= (static_cast<uint32_t>(physTileY) & 0xFFu) << 9u;
		val |= (static_cast<uint32_t>(actualMip) & 0x0Fu) << 17u;
		if (isExactMip)
		{
			val |= EXACT_MIP_BIT;
		}
		return PageTableEntry{ val };
	}

	constexpr bool is_resident() const noexcept
	{
		return (raw & RESIDENT_BIT) != 0;
	}

	constexpr bool is_exact_mip() const noexcept
	{
		return (raw & EXACT_MIP_BIT) != 0;
	}

	constexpr uint8_t get_physical_x() const noexcept
	{
		return static_cast<uint8_t>((raw >> 1u) & 0xFFu);
	}

	constexpr uint8_t get_physical_y() const noexcept
	{
		return static_cast<uint8_t>((raw >> 9u) & 0xFFu);
	}

	constexpr uint8_t get_actual_mip() const noexcept
	{
		return static_cast<uint8_t>((raw >> 17u) & 0x0Fu);
	}

	constexpr PageTableEntry as_fallback() const noexcept
	{
		return PageTableEntry{ raw & ~EXACT_MIP_BIT };
	}
};
static_assert(sizeof(PageTableEntry) == 4, "PageTableEntry must be 4 bytes");

// 32 bit packed feedback request written by shaders
//	0-11: textureId 0-4095
//	12-19: tileX 0-255
//	20-27: tileY 0-255
//	28-31: mipLevel 0-15
struct VTFeedbackRequest
{
	uint32_t raw{ 0 };

	static constexpr uint32_t pack(uint16_t textureId, uint8_t tileX, uint8_t tileY, uint8_t mipLevel) noexcept
	{
		return (static_cast<uint32_t>(textureId) & 0x0FFFu) |
			((static_cast<uint32_t>(tileX) & 0xFFu) << 12u) |
			((static_cast<uint32_t>(tileY) & 0xFFu) << 20u) |
			((static_cast<uint32_t>(mipLevel) & 0x0Fu) << 28u);
	}

	static constexpr VirtualPageID unpack(uint32_t packed) noexcept
	{
		VirtualPageID page{};
		page.textureId = static_cast<uint16_t>(packed & 0xFFFu);
		page.tileX = static_cast<uint16_t>((packed >> 12u) & 0xFFu);
		page.tileY = static_cast<uint16_t>((packed >> 20u) & 0xFFu);
		page.mipLevel = static_cast<uint8_t>((packed >> 28u) & 0x0Fu);
		return page;
	}
};

// metadata per registered virtual texture
#pragma pack(push, 1)
struct GPUVirtualTextureDesc
{
	uint32_t pageTableOffsetX{ 0 };    // base X tile offset in global page table mip 0
	uint32_t pageTableOffsetY{ 0 };    // base Y tile offset in global page table mip 0
	uint32_t virtualWidth{ 0 };        // full virtual width in texels at mip 0
	uint32_t virtualHeight{ 0 };       // full virtual height in texels at mip 0
	uint32_t tilesX{ 0 };              // number of horizontal tiles at mip 0
	uint32_t tilesY{ 0 };              // number of vertical tiles at mip 0
	uint32_t maxMipLevel{ 0 };         // coarsest mip level index (mipCount - 1)
	uint32_t fallbackPageEntry{ 0 };   // pinned tail mip PageTableEntry guaranteed resident
};
#pragma pack(pop)
static_assert(sizeof(GPUVirtualTextureDesc) == 32, "GPUVirtualTextureDesc must be 32 bytes std430 aligned");

// coordinates of a single allocated slot inside physical tile pool
struct PhysicalSlotCoord
{
	uint32_t slotIndex{ VT_INVALID_PHYSICAL_SLOT };
	uint8_t tileX{ 0 };
	uint8_t tileY{ 0 };

	constexpr bool is_valid() const noexcept
	{
		return slotIndex != VT_INVALID_PHYSICAL_SLOT;
	}
};

// metrics for imgui debugging/profiling
struct VirtualTextureMetrics
{
	uint32_t totalPhysicalSlots{ 0 };
	uint32_t usedPhysicalSlots{ 0 };
	uint32_t pinnedPhysicalSlots{ 0 };
	uint32_t registeredVirtualTextures{ 0 };
	uint32_t allocatedPageTableTilesMip0{ 0 };
	uint32_t totalPageTableCapacityMip0{ 0 };
	uint64_t totalCacheHits{ 0 };
	uint64_t totalCacheMisses{ 0 };
	uint64_t totalEvictions{ 0 };
	uint64_t physicalAtlasVramBytes{ 0 };
	uint64_t pageTableVramBytes{ 0 };
};