#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

struct RGTextureHandle
{
	uint32_t index{ UINT32_MAX };
	bool is_valid() const { return index != UINT32_MAX; };
	bool operator==(const RGTextureHandle& other) const { return index == other.index; }
};

struct RGBufferHandle
{
	uint32_t index{ UINT32_MAX };
	bool is_valid() const { return index != UINT32_MAX; }
	bool operator==(const RGBufferHandle& other) const { return index == other.index; }
};

enum class RGResourceUsage : uint32_t
{
	None = 0,
	ShaderRead = 1 << 0,
	StorageRead = 1 << 1,
	StorageWrite = 1 << 2,
	ColorAttachment = 1 << 3,
	DepthAttachment = 1 << 4,
	IndirectArgs = 1 << 5,
	TransferSrc = 1 << 6,
	TransferDst = 1 << 7,
	Present = 1 << 8
};

inline RGResourceUsage operator|(RGResourceUsage a, RGResourceUsage b)
{
	return static_cast<RGResourceUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline bool operator&(RGResourceUsage a, RGResourceUsage b)
{
	return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

// https://dev.epicgames.com/documentation/unreal-engine/render-dependency-graph-in-unreal-engine

struct RGBufferPassParameters
{
	// inputs
	RGTextureHandle position;
	RGTextureHandle normal;
	RGTextureHandle albedo;
	RGTextureHandle metalRough;
	RGTextureHandle emissive;
	RGTextureHandle depth;
	
	// outputs
	RGBufferHandle gpuObjectBuffer;
	RGBufferHandle gpuIndirectBuffer;
};

struct ShadowPassParameters
{
	// inputs
	RGBufferHandle gpuObjectBuffer;
	RGBufferHandle gpuIndirectBuffer;

	// outputs
	RGTextureHandle shadowMap;
};

struct PointShadowPassParameters
{
	// inputs
	RGBufferHandle gpuObjectBuffer;
	RGBufferHandle gpuIndirectBuffer;

	// outputs
	RGTextureHandle pointShadowMap;
};

struct GeometryPassParameters
{
	// inputs
	RGBufferHandle gpuObjectBuffer;
	RGBufferHandle gpuIndirectBuffer;

	// outputs
	RGTextureHandle gPosition;
	RGTextureHandle gNormal;
	RGTextureHandle gAlbedo;
	RGTextureHandle gMetalRough;
	RGTextureHandle gEmissive;
	RGTextureHandle depth;
};

struct SSAOPassParameters
{
	// inputs
	RGTextureHandle gPosition;
	RGTextureHandle gNormal;

	// outputs
	RGTextureHandle ssao;
};

struct SSAOBlurPassParameters
{
	// inputs
	RGTextureHandle ssao;

	// outputs
	RGTextureHandle ssaoBlur;
};

struct DeferredPassParameters
{
	// inputs
	RGTextureHandle gPosition;
	RGTextureHandle gNormal;
	RGTextureHandle gAlbedo;
	RGTextureHandle gMetalRough;
	RGTextureHandle gEmissive;
	RGTextureHandle depth;
	RGTextureHandle shadowMap;
	RGTextureHandle ssao;

	// outputs
	RGTextureHandle sceneColor;
};


struct SkyboxPassParameters
{
	// inputs
	RGTextureHandle depth;

	// outputs
	RGTextureHandle sceneColor;
	RGTextureHandle sceneDepth;
};