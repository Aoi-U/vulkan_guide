const uint VT_INVALID_INDEX = 0xFFFFu;
const uint VT_MAX_FEEDBACK_ITEMS = 16384u;
const float VT_LOGICAL_TILE_SIZE = 128.0;
const float VT_TILE_BORDER = 4.0;
const float VT_PHYSICAL_TILE_SIZE = 136.0;

// physical atlas array layer indices
const float VT_LAYER_ALBEDO = 0.0;
const float VT_LAYER_NORMAL = 1.0;
const float VT_LAYER_METALROUGH_AO = 2.0;

// debug modes
const int VT_DEBUG_NONE = 0;
const int VT_DEBUG_MIP_HEATMAP = 1;
const int VT_DEBUG_RESIDENCY_STATUS = 2;
const int VT_DEBUG_PHYSICAL_GRID = 3;

struct GPUVirtualTextureDesc
{
	uint pageTableOffsetX;
	uint pageTableOffsetY;
	uint virtualWidth;
	uint virtualHeight;
	uint tilesX;
	uint tilesY;
	uint maxMipLevel;
	uint fallbackPageEntry;
};

layout (set = 0, binding = 8) uniform usampler2D vtPageTable;
layout (set = 0, binding = 9) uniform sampler2DArray vtPhysicalAtlas;
layout (std430, set = 0, binding = 10) readonly buffer VTDescBuffer
{
	GPUVirtualTextureDesc descriptors[];
} vtDescBuffer;
layout (std430, set = 0, binding = 11) buffer VTFeedbackBuffer
{
	uint requestCount;
	uint pad0;
	uint pad1;
	uint pad2;
	uint requests[];
} vtFeedback;

vec3 vt_srgb_to_linear(vec3 srgb)
{
	bvec3 cutoff = lessThanEqual(srgb, vec3(0.04045));
	vec3 lower = srgb / 12.92;
	vec3 higher = pow((srgb + 0.055) / 1.055, vec3(2.4));
	return mix(higher, lower, cutoff);
}

uint vt_pack_feedback_request(uint textureId, uint tileX, uint tileY, uint mipLevel)
{
	return (textureId & 0x0FFFu) |
	       ((tileX    & 0xFFu) << 12u) |
	       ((tileY    & 0xFFu) << 20u) |
	       ((mipLevel & 0x0Fu) << 28u);
}

void vt_emit_feedback(uint textureId, uint tileX, uint tileY, uint mipLevel)
{
	uint subsample = max(1u, uint(sceneData.vtParams.x));
	uvec2 pix = uvec2(gl_FragCoord.xy);
	uint frameIdx = uint(sceneData.vtParams.w);

	uvec2 jitter = uvec2(frameIdx % subsample, (frameIdx / subsample) % subsample);
	if ((pix.x % subsample) != jitter.x || (pix.y % subsample) != jitter.y) 
	{
		return;
	}

	uint packedReq = vt_pack_feedback_request(textureId, tileX, tileY, mipLevel);
	if (packedReq == 0u)
	{
		return;
	}

	uint slot = atomicAdd(vtFeedback.requestCount, 1u);
	if (slot < VT_MAX_FEEDBACK_ITEMS)
	{
		vtFeedback.requests[slot] = packedReq;
	}
}

uint vt_compute_mip_level(vec2 ddxUV, vec2 ddyUV, vec2 virtualSize, uint maxMipLevel)
{
	vec2 dxTexels = ddxUV * virtualSize;
	vec2 dyTexels = ddyUV * virtualSize;
	float deltaMaxSqr = max(dot(dxTexels, dxTexels), dot(dyTexels, dyTexels));
	float rawMip = 0.5 * log2(max(deltaMaxSqr, 1e-8)) + sceneData.vtParams.z;
	return uint(clamp(floor(rawMip + 0.5), 0.0, float(maxMipLevel)));
}

// Color lut for visualizing virtual mip levels 
vec3 vt_debug_mip_color(uint mip)
{
	const vec3 lut[8] = vec3[](
		vec3(0.10, 0.85, 1.00), // mip 0: cyan
		vec3(0.15, 0.90, 0.35), // mip 1: green
		vec3(0.95, 0.90, 0.15), // mip 2: yellow
		vec3(1.00, 0.50, 0.10), // mip 3: orange
		vec3(0.95, 0.15, 0.15), // mip 4: red
		vec3(0.85, 0.15, 0.85), // mip 5: magenta
		vec3(0.45, 0.20, 0.95), // mip 6: purple
		vec3(1.00, 1.00, 1.00)  // mip 7+: white
	);
	return lut[min(mip, 7u)];
}

void vt_sample_pbr_layers(uint textureId, vec2 rawUV, out vec4 outAlbedo, out vec4 outNormal, out vec4 outMetalRoughOccl)
{
	GPUVirtualTextureDesc desc = vtDescBuffer.descriptors[textureId];

	// fallback if descriptor metadata has not been synced yet
	if (desc.virtualWidth == 0u || desc.virtualHeight == 0u) {
		outAlbedo         = vec4(1.0);
		outNormal         = vec4(0.5, 0.5, 1.0, 1.0);
		outMetalRoughOccl = vec4(1.0);
		return;
	}

	vec2 ddxUV = dFdx(rawUV);
	vec2 ddyUV = dFdy(rawUV);
	vec2 wrappedUV = fract(rawUV);

	uint requestedMip = vt_compute_mip_level(
		ddxUV,
		ddyUV,
		vec2(float(desc.virtualWidth), float(desc.virtualHeight)),
		desc.maxMipLevel
	);

	// compute local tile coordinate at requestedMip
	uvec2 reqMipTiles = max(uvec2(1u), uvec2(desc.tilesX, desc.tilesY) >> requestedMip);
	uvec2 localTile = min(uvec2(floor(wrappedUV * vec2(reqMipTiles))), reqMipTiles - uvec2(1u));

	// emit gpu feedback request
	vt_emit_feedback(textureId, localTile.x, localTile.y, requestedMip);

	// page table lookup 
	uvec2 globalPageCoord = (uvec2(desc.pageTableOffsetX, desc.pageTableOffsetY) >> requestedMip) + localTile;
	uint pageEntry = texelFetch(vtPageTable, ivec2(globalPageCoord), int(requestedMip)).r;

	if ((pageEntry & 1u) == 0u) {
		pageEntry = desc.fallbackPageEntry;
	}

	uint physTileX  = (pageEntry >> 1u)  & 0xFFu;
	uint physTileY  = (pageEntry >> 9u)  & 0xFFu;
	uint actualMip  = (pageEntry >> 17u) & 0x0Fu;
	bool isExactMip = ((pageEntry >> 21u) & 1u) != 0u && (actualMip == requestedMip);

	// translate wrappedUV into resolved actualMip tiles interior [4 .. 132)
	vec2 actualMipTiles = vec2(max(uvec2(1u), uvec2(desc.tilesX, desc.tilesY) >> actualMip));
	vec2 inTileUV = fract(wrappedUV * actualMipTiles);

	vec2 atlasSize = vec2(textureSize(vtPhysicalAtlas, 0).xy);
	vec2 atlasPixelPos = vec2(float(physTileX), float(physTileY)) * VT_PHYSICAL_TILE_SIZE
	                   + vec2(VT_TILE_BORDER)
	                   + inTileUV * VT_LOGICAL_TILE_SIZE;
	vec2 atlasUV = atlasPixelPos / atlasSize;

	// scale continuous UV gradients to physical atlas space 
	vec2 uvToAtlasScale = (actualMipTiles * VT_LOGICAL_TILE_SIZE) / atlasSize;
	vec2 maxGrad = vec2(VT_TILE_BORDER) / atlasSize;
	vec2 gradX = clamp(ddxUV * uvToAtlasScale, -maxGrad, maxGrad);
	vec2 gradY = clamp(ddyUV * uvToAtlasScale, -maxGrad, maxGrad);

	vec4 rawAlbedoSample = textureGrad(vtPhysicalAtlas, vec3(atlasUV, VT_LAYER_ALBEDO), gradX, gradY);
	outAlbedo         = vec4(vt_srgb_to_linear(rawAlbedoSample.rgb), rawAlbedoSample.a);
	outNormal         = textureGrad(vtPhysicalAtlas, vec3(atlasUV, VT_LAYER_NORMAL),        gradX, gradY);
	outMetalRoughOccl = textureGrad(vtPhysicalAtlas, vec3(atlasUV, VT_LAYER_METALROUGH_AO), gradX, gradY);

	// debug visualizations 
	int debugMode = int(sceneData.vtParams.y);
	if (debugMode != VT_DEBUG_NONE) {
		vec2 borderDist = min(inTileUV, vec2(1.0) - inTileUV) * VT_LOGICAL_TILE_SIZE;
		float gridLine = (min(borderDist.x, borderDist.y) < 1.25) ? 1.0 : 0.0;

		if (debugMode == VT_DEBUG_MIP_HEATMAP) {
			vec3 mipCol = vt_debug_mip_color(actualMip);
			outAlbedo.rgb = mix(outAlbedo.rgb * 0.25 + mipCol * 0.75, vec3(1.0), gridLine * 0.8);
		}
		else if (debugMode == VT_DEBUG_RESIDENCY_STATUS) {
			// green = exact requested mip is resident; orange/red = sampling coarser fallback mip
			vec3 statusCol = isExactMip ? vec3(0.15, 0.90, 0.30) : vec3(1.00, 0.25, 0.10);
			outAlbedo.rgb = mix(outAlbedo.rgb * 0.3 + statusCol * 0.7, vec3(1.0), gridLine * 0.7);
		}
		else if (debugMode == VT_DEBUG_PHYSICAL_GRID) {
			vec3 slotColor = vec3(
				float(physTileX) / 32.0,
				float(physTileY) / 32.0,
				float((physTileX ^ physTileY) & 7u) / 7.0
			);
			outAlbedo.rgb = mix(slotColor, vec3(1.0, 1.0, 0.0), gridLine);
		}
	}
}