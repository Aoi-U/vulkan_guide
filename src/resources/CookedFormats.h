#pragma once

#include <graphics/utils/vk_types.h>
#include <cstdint>

static constexpr uint32_t GMESH_MAGIC = 0x4D534847; // "GMSH" in little endian
static constexpr uint32_t GTEX_MAGIC = 0x58455447; // "GTEX" in little endian
static constexpr uint32_t VTEX_MAGIC = 0x58455456; // "VTEX" in little endian
static constexpr uint32_t MAX_MESH_LODS = 4;
static constexpr uint32_t MAX_VTEX_MIPS = 12;

#pragma pack(push, 1)
struct CookedMeshLODInfo
{
	uint32_t indexOffset{ 0 }; // offset info cooked index array in indices
	uint32_t indexCount{ 0 }; // number of indices for this lod
	float screenSizeThreshold{ 1.0f }; // screen space threshold for gpu lod selection
};

struct CookedMeshHeader
{
	uint32_t magic{ GMESH_MAGIC };
	uint32_t version{ 1 };
	Bounds bounds{}; // precomputed aabb and bounding sphere
	uint32_t vertexCount{ 0 };
	uint32_t totalIndexCount{ 0 }; // total number of indices for all lods
	uint32_t lodCount{ 1 }; 
	CookedMeshLODInfo lods[MAX_MESH_LODS]{};
	uint8_t hasPickingData{ 0 };
};

struct CookedTextureHeader
{
	uint32_t magic{ GTEX_MAGIC };
	uint32_t version{ 1 };
	uint32_t width{ 1 };
	uint32_t height{ 1 };
	uint32_t depth{ 1 };
	uint32_t mipLevels{ 1 };
	uint32_t arrayLayers{ 1 };
	VkFormat format{ VK_FORMAT_R8G8B8A8_UNORM };
	VkFilter magFilter{ VK_FILTER_LINEAR };
	VkFilter minFilter{ VK_FILTER_LINEAR };
	VkSamplerMipmapMode mipmapMode{ VK_SAMPLER_MIPMAP_MODE_LINEAR };
	VkSamplerAddressMode addressModeU{ VK_SAMPLER_ADDRESS_MODE_REPEAT };
	VkSamplerAddressMode addressModeV{ VK_SAMPLER_ADDRESS_MODE_REPEAT };
	uint64_t payloadSizeBytes{ 0 }; // tightly packed pixel data
};

// per mip metadata inside cooked .vtex file
struct CookedVTexMipInfo
{
	uint32_t width{ 1 }; // virtual width in texels for this mip level
	uint32_t height{ 1 }; // virtual height in texels for this mip level
	uint32_t tilesX{ 1 }; // horizontal tile count for this mip level
	uint32_t tilesY{ 1 }; // vertical tile count for this mip level
	uint32_t firstTileIndex{ 0 }; // base index into CookedVTexTileEntry table
};

// per tile lookup entry following CookedVTexHeader in .vtex file
struct CookedVTexTileEntry
{
	uint64_t byteOffset{ 0 }; // absolute byte offset from beginning of .vtex file
	uint32_t byteSize{ 0 }; // payload size in bytes
	uint32_t flags{ 0 }; // reserved flags 
};

struct CookedVTexHeader
{
	uint32_t magic{ VTEX_MAGIC };
	uint32_t version{ 1 };
	uint32_t virtualWidth{ 1 };
	uint32_t virtualHeight{ 1 };
	uint32_t logicalTileSize{ 128 };
	uint32_t borderSize{ 4 };
	uint32_t physicalTileSize{ 136 };
	uint32_t mipLevels{ 1 };
	uint32_t totalTileCount{ 0 };
	VkFormat format{ VK_FORMAT_R8G8B8A8_UNORM };
	VkSamplerAddressMode addressModeU{ VK_SAMPLER_ADDRESS_MODE_REPEAT };
	VkSamplerAddressMode addressModeV{ VK_SAMPLER_ADDRESS_MODE_REPEAT };
	CookedVTexMipInfo mips[MAX_VTEX_MIPS];
};

#pragma pack(pop)