#include "AssetImporter.h"

#include <graphics/vt/VirtualTextureTypes.h>
#include <resources/AssetRegistry.h>
#include <resources/AssetSerializer.h>
#include <meshoptimizer/src/meshoptimizer.h>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/parser.hpp>
#include <fastgltf/tools.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <stb_image.h>
#include <fmt/core.h>
#include <fstream>
#include <cmath>
#include <algorithm>

namespace // helpers
{

	VkFilter extract_filter(fastgltf::Filter filter) {
		switch (filter) {
		case fastgltf::Filter::Nearest:
		case fastgltf::Filter::NearestMipMapNearest:
		case fastgltf::Filter::NearestMipMapLinear:
			return VK_FILTER_NEAREST;
		default:
			return VK_FILTER_LINEAR;
		}
	}

	VkSamplerMipmapMode extract_mipmap(fastgltf::Filter filter) {
		switch (filter) {
		case fastgltf::Filter::NearestMipMapNearest:
		case fastgltf::Filter::LinearMipMapNearest:
			return VK_SAMPLER_MIPMAP_MODE_NEAREST;
		default:
			return VK_SAMPLER_MIPMAP_MODE_LINEAR;
		}
	}

	VkSamplerAddressMode extract_wrap(fastgltf::Wrap wrap) {
		switch (wrap) {
		case fastgltf::Wrap::ClampToEdge:
			return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		case fastgltf::Wrap::MirroredRepeat:
			return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
		default:
			return VK_SAMPLER_ADDRESS_MODE_REPEAT;
		}
	}

	// compute MikkTSpace orthogonalized vertex tangents when glTF primitive omits TANGENT
	void compute_missing_tangents(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices)
	{
		for (auto& v : vertices) {
			v.tangent = glm::vec4(0.0f);
		}

		for (size_t j = 0; j + 2 < indices.size(); j += 3) {
			Vertex& v0 = vertices[indices[j + 0]];
			Vertex& v1 = vertices[indices[j + 1]];
			Vertex& v2 = vertices[indices[j + 2]];

			glm::vec3 edge1 = v1.position - v0.position;
			glm::vec3 edge2 = v2.position - v0.position;
			glm::vec2 deltaUV1 = glm::vec2(v1.uv_x, v1.uv_y) - glm::vec2(v0.uv_x, v0.uv_y);
			glm::vec2 deltaUV2 = glm::vec2(v2.uv_x, v2.uv_y) - glm::vec2(v0.uv_x, v0.uv_y);

			float det = (deltaUV1.x * deltaUV2.y - deltaUV2.x * deltaUV1.y);
			glm::vec3 tangent(1.0f, 0.0f, 0.0f);
			if (std::abs(det) > 1e-6f) {
				float f = 1.0f / det;
				tangent.x = f * (deltaUV2.y * edge1.x - deltaUV1.y * edge2.x);
				tangent.y = f * (deltaUV2.y * edge1.y - deltaUV1.y * edge2.y);
				tangent.z = f * (deltaUV2.y * edge1.z - deltaUV1.y * edge2.z);
			}

			v0.tangent += glm::vec4(tangent, 0.0f);
			v1.tangent += glm::vec4(tangent, 0.0f);
			v2.tangent += glm::vec4(tangent, 0.0f);
		}

		for (auto& v : vertices) {
			auto t = glm::vec3(v.tangent);
			glm::vec3 n = v.normal;
			glm::vec3 orthoDir = t - n * glm::dot(n, t);
			glm::vec3 ortho = (glm::length(orthoDir) > 1e-6f) ? glm::normalize(orthoDir) : glm::vec3(1.0f, 0.0f, 0.0f);
			v.tangent = glm::vec4(ortho, 1.0f);
		}
	}

	// wrap or clamp 1d integer texel coord based on mode
	int32_t wrap_texel_coord(const int32_t coord, const int32_t size, const VkSamplerAddressMode mode) noexcept
	{
		if (size <= 1)
			return 0;

		switch (mode)
		{
		case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:
		case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:
			return std::clamp(coord, 0, size - 1);
		case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:
		{
			const int32_t period = 2 * size;
			int32_t m = coord % period;
			if (m < 0)
				m += period;
			return (m < size) ? m : (period - 1 - m);
		}
		case VK_SAMPLER_ADDRESS_MODE_REPEAT:
		default:
		{
			int32_t m = coord % size;
			return (m < 0) ? (m + size) : m;
		}
		}
	}

	float srgb_to_linear(const uint8_t byteVal) noexcept
	{
		const float s = static_cast<float>(byteVal) * (1.0f / 255.0f);
		return (s <= 0.04045f) ? (s / 12.92f) : std::pow((s + 0.055f) / 1.055f, 2.4f);
	}

	uint8_t linear_to_srgb(const float linearVal) noexcept
	{
		const float clamped = std::clamp(linearVal, 0.0f, 1.0f);
		const float s = (clamped <= 0.0031308f) ? (clamped * 12.92f) : (1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f);
		return static_cast<uint8_t>(std::clamp(s * 255.0f + 0.5f, 0.0f, 255.0f));
	}

	// downsample rgba8 image by 2x using 2x2 box filter (gamma-corrected if isSRGB is true)
	std::vector<uint8_t> downsample_mip_2x_rgba8(const uint8_t* srcPixels, const uint32_t srcWidth, const uint32_t srcHeight,
		const uint32_t dstWidth, const uint32_t dstHeight, const bool isSRGB)
	{
		std::vector<uint8_t> dstPixels(static_cast<size_t>(dstWidth) * dstHeight * 4u);

		for (uint32_t y = 0; y < dstHeight; y++)
		{
			const uint32_t sy0 = std::min(srcHeight - 1u, y * 2u);
			const uint32_t sy1 = std::min(srcHeight - 1u, y * 2u + 1u);

			for (uint32_t x = 0; x < dstWidth; x++)
			{
				const uint32_t sx0 = std::min(srcWidth - 1u, x * 2u);
				const uint32_t sx1 = std::min(srcWidth - 1u, x * 2u + 1u);

				const uint8_t* p00 = &srcPixels[(static_cast<size_t>(sy0) * srcWidth + sx0) * 4u];
				const uint8_t* p10 = &srcPixels[(static_cast<size_t>(sy0) * srcWidth + sx1) * 4u];
				const uint8_t* p01 = &srcPixels[(static_cast<size_t>(sy1) * srcWidth + sx0) * 4u];
				const uint8_t* p11 = &srcPixels[(static_cast<size_t>(sy1) * srcWidth + sx1) * 4u];

				uint8_t* outPx = &dstPixels[(static_cast<size_t>(y) * dstWidth + x) * 4u];

				if (isSRGB)
				{
					for (int c = 0; c < 3; c++)
					{
						float avgLinear = 0.25f * (
							srgb_to_linear(p00[c]) +
							srgb_to_linear(p10[c]) +
							srgb_to_linear(p01[c]) +
							srgb_to_linear(p11[c])
							);
						outPx[c] = linear_to_srgb(avgLinear);
					}
					outPx[3] = static_cast<uint8_t>((uint32_t(p00[3]) + p10[3] + p01[3] + p11[3] + 2u) >> 2u);
				}
				else
				{
					for (int c = 0; c < 4; c++)
						outPx[c] = static_cast<uint8_t>((uint32_t(p00[c]) + p10[c] + p01[c] + p11[c] + 2u) >> 2u);
				}
			}
		}

		return dstPixels;
	}

	// extract single 136x136 tile with 4 pixel border around all sides
	void extract_padded_tile_rgba8(const uint8_t* mipPixels, uint32_t mipWidth, uint32_t mipHeight, uint32_t tileX, uint32_t tileY,
		VkSamplerAddressMode addressU, VkSamplerAddressMode addressV, uint8_t* outPhysicalTilePixels)
	{
		const int32_t w = static_cast<int32_t>(mipWidth);
		const int32_t h = static_cast<int32_t>(mipHeight);

		// for mips smaller than VT_LOGICAL_TILE_SIZE, single tile covers entire uv space in shader
		// scaling logical coord [0..127] onto [0..w - 1] ensures sub-128 mips map 1:1 to uv [0, 1)
		const bool scaleSubTileX = (mipWidth < VT_LOGICAL_TILE_SIZE);
		const bool scaleSubTileY = (mipHeight < VT_LOGICAL_TILE_SIZE);

		for (uint32_t py = 0; py < VT_PHYSICAL_TILE_SIZE; py++)
		{
			const int32_t localY = static_cast<int32_t>(py) - static_cast<int32_t>(VT_TILE_BORDER);
			int32_t srcY = 0;

			if (scaleSubTileY)
			{
				// map [-4..131] onto [-border..h + border)
				const int32_t scaledY = static_cast<int32_t>(std::floor((
					static_cast<float>(localY) + 0.5f) * static_cast<float>(h) / static_cast<float>(VT_LOGICAL_TILE_SIZE)));
				srcY = wrap_texel_coord(scaledY, h, addressV);
			}
			else
			{
				const int32_t globalY = static_cast<int32_t>(tileY * VT_LOGICAL_TILE_SIZE) + localY;
				srcY = wrap_texel_coord(globalY, h, addressV);
			}

			for (uint32_t px = 0; px < VT_PHYSICAL_TILE_SIZE; px++)
			{
				const int32_t localX = static_cast<int32_t>(px) - static_cast<int32_t>(VT_TILE_BORDER);
				int32_t srcX = 0;

				if (scaleSubTileX)
				{
					const int32_t scaledX = static_cast<int32_t>(std::floor(
						(static_cast<float>(localX) + 0.5f) * static_cast<float>(w) / static_cast<float>(VT_LOGICAL_TILE_SIZE)));
					srcX = wrap_texel_coord(scaledX, w, addressU);
				}
				else
				{
					const int32_t globalX = static_cast<int32_t>(tileX * VT_LOGICAL_TILE_SIZE) + localX;
					srcX = wrap_texel_coord(globalX, w, addressU);
				}

				const uint8_t* srcTexel = &mipPixels[(static_cast<size_t>(srcY) * mipWidth + static_cast<uint32_t>(srcX)) * 4u];
				uint8_t* dstTexel = &outPhysicalTilePixels[(static_cast<size_t>(py) * VT_PHYSICAL_TILE_SIZE + px) * 4u];
				memcpy(dstTexel, srcTexel, 4u);
			}
		}
	}
}


void AssetImporter::cook_missing_assets()
{
	const std::vector<AssetMetadata> allAssets = AssetRegistry::get().get_all_assets();
	for (const AssetMetadata& meta : allAssets) {
		if (meta.is_sub_asset())
			continue;

		if (meta.type == AssetType::Texture) { // cook missing textures
			if (!meta.cookedPath.empty() && !std::filesystem::exists(meta.cookedPath)) {
				cook_texture(meta);
			}
		}
		else if (meta.type == AssetType::Model) {
			// check if .prefab or subassets need cooking
			std::filesystem::path prefabPath = meta.virtualPath;
			prefabPath.replace_extension(".prefab");
			bool needsImport = !std::filesystem::exists(prefabPath) || meta.subAssets.empty();

			if (!needsImport) {
				for (const SubAssetEntry& sub : meta.subAssets) {
					if (sub.type == AssetType::Texture) {
						std::filesystem::path gtex = std::filesystem::path(".cache/textures") / (sub.uuid.to_string() + ".gtex");
						if (!std::filesystem::exists(gtex)) {
							needsImport = true;
							break;
						}
					}
					else if (sub.type == AssetType::Mesh) {
						std::filesystem::path gmesh = std::filesystem::path(".cache/meshes") / (sub.uuid.to_string() + ".gmesh");
						if (!std::filesystem::exists(gmesh)) {
							needsImport = true;
							break;
						}
					}
					else if (sub.type == AssetType::Material) {
						std::filesystem::path matPath = meta.virtualPath.parent_path() / "materials" / (sub.name + ".mat");
						MaterialAssetData matData{};
						if (!std::filesystem::exists(matPath) || !AssetSerializer::load_material_file(matPath, matData) || matData.uuid != sub.uuid) {
							needsImport = true;
							break;
						}
						auto tex_missing = [](const UUID& texUuid) {
							if (!texUuid.is_valid()) return false;
							std::filesystem::path gtex = std::filesystem::path(".cache/textures") / (texUuid.to_string() + ".gtex");
							return !std::filesystem::exists(gtex);
							};
						if (tex_missing(matData.albedoTextureUUID) ||
							tex_missing(matData.normalTextureUUID) ||
							tex_missing(matData.metalRoughTextureUUID) ||
							tex_missing(matData.occlusionTextureUUID) ||
							tex_missing(matData.emissiveTextureUUID)) {
							needsImport = true;
							break;
						}
					}
					else if (const AssetMetadata* subMeta = AssetRegistry::get().get_metadata(sub.uuid)) {
						if (!subMeta->cookedPath.empty() && !std::filesystem::exists(subMeta->cookedPath)) {
							needsImport = true;
							break;
						}
					}
				}
			}

			if (needsImport) {
				import_model(meta);
			}
		}
	}
}

bool AssetImporter::cook_texture(const AssetMetadata& texMeta)
{
	if (texMeta.textureSettings.virtualTexture || texMeta.cookedPath.extension() == ".vtex")
		return cook_virtual_texture(texMeta);

	if (!std::filesystem::exists(texMeta.virtualPath)) {
		fmt::println("AssetImporter: source texture missing: {}", texMeta.virtualPath.string());
		return false;
	}

	int width = 0, height = 0, channels = 0;
	stbi_uc* pixels = stbi_load(texMeta.virtualPath.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
	if (!pixels) {
		fmt::println("AssetImporter: failed to load pixels from file '{}'", texMeta.virtualPath.string());
		return false;
	}

	const TextureImportSettings& settings = texMeta.textureSettings;
	uint32_t mipLevels = settings.generateMips ? static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1u : 1u;

	uint64_t payloadBytes = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4ull; // RGBA8

	// write .gtex header and pixel data
	CookedTextureHeader header{};
	header.magic = GTEX_MAGIC;
	header.version = 1;
	header.width = static_cast<uint32_t>(width);
	header.height = static_cast<uint32_t>(height);
	header.depth = 1;
	header.mipLevels = mipLevels;
	header.arrayLayers = 1;
	header.format = settings.sRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
	header.magFilter = settings.magFilter;
	header.minFilter = settings.minFilter;
	header.mipmapMode = settings.mipmapMode;
	header.addressModeU = settings.addressModeU;
	header.addressModeV = settings.addressModeV;
	header.payloadSizeBytes = payloadBytes;

	std::filesystem::path targetPath = texMeta.cookedPath.empty() ? (std::filesystem::path(".cache/textures") / (texMeta.uuid.to_string() + ".gtex")) : texMeta.cookedPath;

	bool success = write_gtex_file(targetPath, header, std::span<const uint8_t>(pixels, payloadBytes));
	stbi_image_free(pixels);

	if (success) {
		fmt::println("AssetImporter: Cooked texture '{}' -> '{}' ({}x{}, {} mips)",
			texMeta.name, targetPath.generic_string(), width, height, mipLevels);
	}

	return success;
}

bool AssetImporter::write_gtex_file(const std::filesystem::path& cookedPath, const CookedTextureHeader& header, std::span<const uint8_t> pixelBytes)
{
	if (cookedPath.has_parent_path()) {
		std::filesystem::create_directories(cookedPath.parent_path());
	}

	std::ofstream out(cookedPath, std::ios::binary | std::ios::trunc);
	if (!out.is_open()) {
		fmt::println("AssetImporter: failed to open '{}' for writing .gtex", cookedPath.string());
		return false;
	}

	out.write(reinterpret_cast<const char*>(&header), sizeof(CookedTextureHeader));
	out.write(reinterpret_cast<const char*>(pixelBytes.data()), static_cast<std::streamsize>(pixelBytes.size_bytes()));

	return out.good();
}

bool AssetImporter::cook_virtual_texture(const AssetMetadata& texMeta)
{
	if (!std::filesystem::exists(texMeta.virtualPath))
	{
		fmt::println("AssetImporter: source virtual texture missing: {}", texMeta.virtualPath.string());
		return false;
	}

	int width = 0, height = 0, channels = 0;
	stbi_uc* pixels = stbi_load(texMeta.virtualPath.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
	if (!pixels)
	{
		fmt::println("AssetImporter: failed to load pixels from file '{}'", texMeta.virtualPath.string());
		return false;
	}

	std::filesystem::path targetPath = texMeta.cookedPath;
	if (targetPath.empty() || targetPath.extension() != ".vtex")
		targetPath = std::filesystem::path(".cache/textures") / (texMeta.uuid.to_string() + ".vtex");

	bool ok = cook_vtex_from_rgba8(targetPath, pixels, static_cast<uint32_t>(width), static_cast<uint32_t>(height), texMeta.textureSettings);

	stbi_image_free(pixels);

	if (ok)
	{
		fmt::println("AssetImporter: cooked virtual texture '{}'", texMeta.name);
	}
	return ok;
}

bool AssetImporter::cook_vtex_from_rgba8(const std::filesystem::path& cookedPath, const uint8_t* mip0Pixels,
	uint32_t width, uint32_t height, const TextureImportSettings& settings)
{
	if (!mip0Pixels || width == 0 || height == 0)
		return false;

	// compute number of virtual tile mip levels so tail mip is a single 1x1 tile (128x128 texel)
	const uint32_t tilesXMip0 = std::max(1u, (width + VT_LOGICAL_TILE_SIZE - 1u) / VT_LOGICAL_TILE_SIZE);
	const uint32_t tilesYMip0 = std::max(1u, (height + VT_LOGICAL_TILE_SIZE - 1u) / VT_LOGICAL_TILE_SIZE);
	const uint32_t maxTileSpan = std::max(tilesXMip0, tilesYMip0);

	uint32_t mipLevels = settings.generateMips ? (static_cast<uint32_t>(std::floor(std::log2(maxTileSpan))) + 1u) : 1u;
	mipLevels = std::clamp(mipLevels, 1u, MAX_VTEX_MIPS);

	// populate .vtex header and per mip layout
	CookedVTexHeader header{};
	header.magic = VTEX_MAGIC;
	header.version = 1;
	header.virtualWidth = width;
	header.virtualHeight = height;
	header.logicalTileSize = VT_LOGICAL_TILE_SIZE;
	header.borderSize = VT_TILE_BORDER;
	header.physicalTileSize = VT_PHYSICAL_TILE_SIZE;
	header.mipLevels = mipLevels;
	header.format = settings.sRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
	header.addressModeU = settings.addressModeU;
	header.addressModeV = settings.addressModeV;

	uint32_t runningTileCount = 0;
	for (uint32_t mip = 0; mip < mipLevels; mip++)
	{
		const uint32_t mipW = std::max(1u, width >> mip);
		const uint32_t mipH = std::max(1u, height >> mip);
		const uint32_t mipTilesX = std::max(1u, tilesXMip0 >> mip);
		const uint32_t mipTilesY = std::max(1u, tilesYMip0 >> mip);

		header.mips[mip] = CookedVTexMipInfo{
			.width = mipW,
			.height = mipH,
			.tilesX = mipTilesX,
			.tilesY = mipTilesY,
			.firstTileIndex = runningTileCount
		};
		runningTileCount += (mipTilesX * mipTilesY);
	}
	header.totalTileCount = runningTileCount;

	if (cookedPath.has_parent_path())
		std::filesystem::create_directories(cookedPath.parent_path());

	std::ofstream out(cookedPath, std::ios::binary | std::ios::trunc);
	if (!out.is_open())
	{
		fmt::println("AssetImporter: failed to open '{}' for writing .vtex", cookedPath.string());
		return false;
	}

	constexpr uint32_t tileByteSize = VT_PHYSICAL_TILE_SIZE * VT_PHYSICAL_TILE_SIZE * 4u;
	const uint64_t payloadStartOffset = sizeof(CookedVTexHeader) + static_cast<uint64_t>(header.totalTileCount) * sizeof(CookedVTexTileEntry);

	// build tile offset table
	std::vector<CookedVTexTileEntry> tileTable(header.totalTileCount);
	for (uint32_t i = 0; i < header.totalTileCount; i++)
	{
		tileTable[i].byteOffset = payloadStartOffset + static_cast<uint64_t>(i) * tileByteSize;
		tileTable[i].byteSize = tileByteSize;
		tileTable[i].flags = 0;
	}

	// write header and tile index table
	out.write(reinterpret_cast<const char*>(&header), sizeof(CookedVTexHeader));
	out.write(reinterpret_cast<const char*>(tileTable.data()), static_cast<std::streamsize>(tileTable.size() * sizeof(CookedVTexTileEntry)));

	// generate each mip level and stream 136x136 tiles to disk
	std::vector<uint8_t> currentMipPixels;
	std::vector<uint8_t> paddedTileBuffer(tileByteSize);

	for (uint32_t mip = 0; mip < mipLevels; mip++)
	{
		const CookedVTexMipInfo& mipInfo = header.mips[mip];
		const uint8_t* activeMipPtr = nullptr;

		if (mip == 0)
			activeMipPtr = mip0Pixels;
		else
		{
			const CookedVTexMipInfo& prevMipInfo = header.mips[mip - 1];
			const uint8_t* prevMipPtr = (mip == 1) ? mip0Pixels : currentMipPixels.data();
			currentMipPixels = downsample_mip_2x_rgba8(prevMipPtr, prevMipInfo.width, prevMipInfo.height, mipInfo.width, mipInfo.height, settings.sRGB);
			activeMipPtr = currentMipPixels.data();
		}

		for (uint32_t ty = 0; ty < mipInfo.tilesY; ty++)
		{
			for (uint32_t tx = 0; tx < mipInfo.tilesX; tx++)
			{
				extract_padded_tile_rgba8(activeMipPtr, mipInfo.width, mipInfo.height, tx, ty, settings.addressModeU, settings.addressModeV, paddedTileBuffer.data());

				out.write(reinterpret_cast<const char*>(paddedTileBuffer.data()), static_cast<std::streamsize>(paddedTileBuffer.size()));
			}
		}
	}

	return out.good();
}

bool AssetImporter::read_vtex_header(const std::filesystem::path& vtexPath, CookedVTexHeader& outHeader,
	std::vector<CookedVTexTileEntry>& outTileTable)
{
	std::ifstream in(vtexPath, std::ios::binary);
	if (!in.is_open())
		return false;

	in.read(reinterpret_cast<char*>(&outHeader), sizeof(CookedVTexHeader));
	if (!in.good() || outHeader.magic != VTEX_MAGIC || outHeader.totalTileCount == 0)
		return false;

	outTileTable.resize(outHeader.totalTileCount);
	in.read(reinterpret_cast<char*>(outTileTable.data()), static_cast<std::streamsize>(outTileTable.size() * sizeof(CookedVTexTileEntry)));

	return in.good();
}

bool AssetImporter::read_vtex_tile(const std::filesystem::path& vtexPath, const CookedVTexHeader& header,
	std::span<const CookedVTexTileEntry> tileTable, uint32_t mipLevel, uint32_t tileX, uint32_t tileY,
	std::vector<uint8_t>& outTilePixels)
{
	if (mipLevel >= header.mipLevels || mipLevel >= MAX_VTEX_MIPS)
		return false;

	const CookedVTexMipInfo& mipInfo = header.mips[mipLevel];
	if (tileX >= mipInfo.tilesX || tileY >= mipInfo.tilesY)
		return false;

	const uint32_t flatTileIndex = mipInfo.firstTileIndex + (tileY * mipInfo.tilesX + tileX);
	if (flatTileIndex >= tileTable.size())
		return false;

	const CookedVTexTileEntry& entry = tileTable[flatTileIndex];
	if (entry.byteSize == 0)
		return false;

	std::ifstream in(vtexPath, std::ios::binary);
	if (in.is_open())
		return false;

	// seek to tile offset 
	in.seekg(static_cast<std::streamoff>(entry.byteOffset), std::ios::beg);
	if (in.good())
		return false;

	// read 136x136 tile into output buffer
	outTilePixels.resize(entry.byteSize);
	in.read(reinterpret_cast<char*>(outTilePixels.data()), static_cast<std::streamsize>(entry.byteSize));
	return in.good();
}

bool AssetImporter::cook_mesh_data(const std::filesystem::path& cookedPath, std::span<const Vertex> rawVertices, std::span<const uint32_t> rawIndices, const MeshImportSettings& settings, Bounds& outBounds)
{
	if (rawVertices.empty() || rawIndices.empty())
		return false;

	// calculate bounds
	glm::vec3 minPos = rawVertices[0].position;
	glm::vec3 maxPos = rawVertices[0].position;
	for (const Vertex& v : rawVertices) {
		minPos = glm::min(minPos, v.position);
		maxPos = glm::max(maxPos, v.position);
	}
	outBounds.origin = (maxPos + minPos) * 0.5f;
	outBounds.extents = (maxPos - minPos) * 0.5f;
	outBounds.sphereRadius = glm::length(outBounds.extents);

	// remap verticies 
	std::vector<unsigned int> remap(rawVertices.size());
	size_t uniqueVertexCount = meshopt_generateVertexRemap(remap.data(), rawIndices.data(), rawIndices.size(), rawVertices.data(), rawVertices.size(), sizeof(Vertex));
	std::vector<uint32_t> lod0Indices(rawIndices.size());
	std::vector<Vertex> optimizedVertices(uniqueVertexCount);

	meshopt_remapIndexBuffer(lod0Indices.data(), rawIndices.data(), rawIndices.size(), remap.data());
	meshopt_remapVertexBuffer(optimizedVertices.data(), rawVertices.data(), rawVertices.size(), sizeof(Vertex), remap.data());

	// optimize vertex cache
	if (settings.optimizeVertexCache) {
		meshopt_optimizeVertexCache(lod0Indices.data(), lod0Indices.data(), lod0Indices.size(), uniqueVertexCount);
	}
	// optimize overdraw with 5% threshold
	if (settings.optimizeOverdraw) {
		meshopt_optimizeOverdraw(lod0Indices.data(), lod0Indices.data(), lod0Indices.size(), &optimizedVertices[0].position.x, uniqueVertexCount, sizeof(Vertex), 1.05f);
	}

	// optimize vertex fetch
	meshopt_optimizeVertexFetch(optimizedVertices.data(), lod0Indices.data(), lod0Indices.size(), optimizedVertices.data(), uniqueVertexCount, sizeof(Vertex));

	// generate lod 0
	CookedMeshHeader header{};
	header.magic = GMESH_MAGIC;
	header.version = 1;
	header.bounds = outBounds;
	header.vertexCount = static_cast<uint32_t>(optimizedVertices.size());
	header.hasPickingData = settings.keepCpuPickingData ? 1 : 0;

	std::vector<uint32_t> allLodIndices;
	allLodIndices.reserve(lod0Indices.size() * 2);

	header.lods[0] = CookedMeshLODInfo{
		.indexOffset = 0,
		.indexCount = static_cast<uint32_t>(lod0Indices.size()),
		.screenSizeThreshold = 1.0f
	};
	allLodIndices.insert(allLodIndices.end(), lod0Indices.begin(), lod0Indices.end());
	header.lodCount = 1;

	// generate lods
	uint32_t targetMaxLods = settings.generateLODs ? std::clamp(settings.maxLODCount, 1u, MAX_MESH_LODS) : 1u;
	constexpr std::array<float, MAX_MESH_LODS> lodTargetRatios = { 1.0f, 0.50f, 0.25f, 0.125f }; // target ratio of original triangle count for each lod
	constexpr std::array<float, MAX_MESH_LODS> lodTargetErrors = { 0.0f, 0.02f, 0.05f, 0.10f }; // target error threshold for each lod
	constexpr std::array<float, MAX_MESH_LODS> lodScreenThresholds = { 1.0f, 0.40f, 0.15f, 0.05f }; // screen size threshold for each lod

	for (uint32_t lod = 1; lod < targetMaxLods; lod++) {
		size_t targetIndexCount = static_cast<size_t>(static_cast<float>(lod0Indices.size()) * lodTargetRatios[lod]) / 3 * 3;
		if (targetIndexCount < 36 || targetIndexCount >= allLodIndices.size()) {
			break; // mesh too small to simplify further
		}

		std::vector<uint32_t> simplifiedIndices(lod0Indices.size());
		float lodError = 0.0f;
		size_t simplifiedCount = meshopt_simplify(
			simplifiedIndices.data(),
			lod0Indices.data(),
			lod0Indices.size(),
			&optimizedVertices[0].position.x,
			optimizedVertices.size(),
			sizeof(Vertex),
			targetIndexCount,
			lodTargetErrors[lod],
			0,
			&lodError
		);

		// stop lod gen if simplification didnt reduce triangle count by at least 15%
		uint32_t prevLodCount = header.lods[lod - 1].indexCount;
		if (simplifiedCount == 0 || simplifiedCount >= static_cast<size_t>(prevLodCount * 0.85f)) {
			break;
		}

		simplifiedIndices.resize(simplifiedCount);
		meshopt_optimizeVertexCache(simplifiedIndices.data(), simplifiedIndices.data(), simplifiedCount, optimizedVertices.size());

		header.lods[lod] = CookedMeshLODInfo{
			.indexOffset = static_cast<uint32_t>(allLodIndices.size()),
			.indexCount = static_cast<uint32_t>(simplifiedCount),
			.screenSizeThreshold = lodScreenThresholds[lod]
		};

		allLodIndices.insert(allLodIndices.end(), simplifiedIndices.begin(), simplifiedIndices.end());
		header.lodCount++;
	}

	header.totalIndexCount = static_cast<uint32_t>(allLodIndices.size());

	// write binary .gmeshfile to .cache/meshes/
	if (cookedPath.has_parent_path()) {
		std::filesystem::create_directories(cookedPath.parent_path());
	}

	std::ofstream out(cookedPath, std::ios::binary | std::ios::trunc);
	if (!out.is_open()) {
		fmt::println("AssetImporter: failed to open '{}' for writing .gmesh", cookedPath.string());
		return false;
	}

	out.write(reinterpret_cast<const char*>(&header), sizeof(CookedMeshHeader));
	out.write(reinterpret_cast<const char*>(optimizedVertices.data()), static_cast<std::streamsize>(optimizedVertices.size() * sizeof(Vertex)));
	out.write(reinterpret_cast<const char*>(allLodIndices.data()), static_cast<std::streamsize>(allLodIndices.size() * sizeof(uint32_t)));

	// write cpu ray picking positions
	if (header.hasPickingData) {
		std::vector<glm::vec3> pickingPositions;
		pickingPositions.reserve(optimizedVertices.size());
		for (const Vertex& v : optimizedVertices) {
			pickingPositions.push_back(v.position);
		}
		out.write(reinterpret_cast<const char*>(pickingPositions.data()), static_cast<std::streamsize>(pickingPositions.size() * sizeof(glm::vec3)));
	}

	return out.good();
}

bool AssetImporter::import_model(const AssetMetadata& modelMeta)
{
	const std::filesystem::path& filePath = modelMeta.virtualPath;
	if (!std::filesystem::exists(filePath)) {
		fmt::println("AssetImporter: model file not found: {}", filePath.string());
		return false;
	}

	// parse gltf/glb container 
	fastgltf::Parser parser{
		fastgltf::Extensions::KHR_materials_transmission |
		fastgltf::Extensions::KHR_materials_ior |
		fastgltf::Extensions::KHR_materials_emissive_strength |
		fastgltf::Extensions::KHR_materials_unlit |
		fastgltf::Extensions::KHR_texture_transform
	};
	constexpr auto gltfOptions =
		fastgltf::Options::DontRequireValidAssetMember |
		fastgltf::Options::AllowDouble |
		fastgltf::Options::LoadGLBBuffers |
		fastgltf::Options::LoadExternalBuffers;

	fastgltf::GltfDataBuffer data;
	if (!data.loadFromFile(filePath)) {
		fmt::println("AssetImporter: failed to read gltf file buffer: {}", filePath.string());
		return false;
	}

	fastgltf::Asset gltf;
	auto type = fastgltf::determineGltfFileType(&data);

	if (type == fastgltf::GltfType::glTF) { // load gltf file
		auto load = parser.loadGLTF(&data, filePath.parent_path(), gltfOptions);
		if (load)
			gltf = std::move(load.get());
		else
			return false;
	}
	else if (type == fastgltf::GltfType::GLB) { // load glb file
		auto load = parser.loadBinaryGLTF(&data, filePath.parent_path(), gltfOptions);
		if (load)
			gltf = std::move(load.get());
		else
			return false;
	}
	else { // invalid file type
		return false;
	}

	// determine srgb textures
	std::vector<bool> isSRGB(gltf.textures.size(), false);
	for (const fastgltf::Material& mat : gltf.materials) {
		if (mat.pbrData.baseColorTexture.has_value())
			isSRGB[mat.pbrData.baseColorTexture.value().textureIndex] = true;

		if (mat.emissiveTexture.has_value())
			isSRGB[mat.emissiveTexture.value().textureIndex] = true;
	}

	// determine texture formats
	std::vector<UUID> imageUUIDs(gltf.images.size(), UUID::null());
	for (size_t i = 0; i < gltf.images.size(); i++) {
		const fastgltf::Image& image = gltf.images[i];
		std::string subTexName = fmt::format("{}_tex_{}", modelMeta.name, i);

		VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
		VkFilter magFilter = VK_FILTER_LINEAR;
		VkFilter minFilter = VK_FILTER_LINEAR;
		VkSamplerMipmapMode mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		VkSamplerAddressMode addressU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		VkSamplerAddressMode addressV = VK_SAMPLER_ADDRESS_MODE_REPEAT;

		for (size_t t = 0; t < gltf.textures.size(); t++) {
			if (gltf.textures[t].imageIndex.value_or(-1) == i) {
				if (isSRGB[t])
					format = VK_FORMAT_R8G8B8A8_SRGB;

				if (gltf.textures[t].samplerIndex.has_value()) {
					const fastgltf::Sampler& sampler = gltf.samplers[gltf.textures[t].samplerIndex.value()];
					magFilter = sampler.magFilter.has_value() ? extract_filter(sampler.magFilter.value()) : VK_FILTER_LINEAR;
					minFilter = sampler.minFilter.has_value() ? extract_filter(sampler.minFilter.value()) : VK_FILTER_LINEAR;
					mipmapMode = sampler.minFilter.has_value() ? extract_mipmap(sampler.minFilter.value()) : VK_SAMPLER_MIPMAP_MODE_LINEAR;
					addressU = extract_wrap(sampler.wrapS);
					addressV = extract_wrap(sampler.wrapT);
				}
				break;
			}
		}

		UUID texUUID = AssetRegistry::get().register_sub_asset(modelMeta.uuid, subTexName, AssetType::Texture);
		if (AssetMetadata* subMeta = AssetRegistry::get().get_metadata_mutable(texUUID)) {
			subMeta->textureSettings.sRGB = (format == VK_FORMAT_R8G8B8A8_SRGB);
			subMeta->textureSettings.magFilter = magFilter;
			subMeta->textureSettings.minFilter = minFilter;
			subMeta->textureSettings.mipmapMode = mipmapMode;
			subMeta->textureSettings.addressModeU = addressU;
			subMeta->textureSettings.addressModeV = addressV;
		}
		imageUUIDs[i] = texUUID;
		std::filesystem::path cookedTexPath = std::filesystem::path(".cache/textures") / (texUUID.to_string() + ".gtex");

		// decode and cook if not already cached on disk
		if (!std::filesystem::exists(cookedTexPath)) {
			int w = 0, h = 0, ch = 0;
			stbi_uc* pixels = nullptr;

			std::visit(fastgltf::visitor{
				[](const auto&) {},
				[&](const fastgltf::sources::URI& uri) {
					std::filesystem::path p = filePath.parent_path() / std::string(uri.uri.path().begin(), uri.uri.path().end());
					pixels = stbi_load(p.string().c_str(), &w, &h, &ch, STBI_rgb_alpha);
				},
				[&](const fastgltf::sources::Vector& vec) {
					pixels = stbi_load_from_memory(vec.bytes.data(), static_cast<int>(vec.bytes.size()), &w, &h, &ch, STBI_rgb_alpha);
				},
				[&](const fastgltf::sources::ByteView& bvBytes) {
					pixels = stbi_load_from_memory(reinterpret_cast<const uint8_t*>(bvBytes.bytes.data()), static_cast<int>(bvBytes.bytes.size()), &w, &h, &ch, STBI_rgb_alpha);
				},
				[&](const fastgltf::sources::BufferView& view) {
					const auto& bv = gltf.bufferViews[view.bufferViewIndex];
					const auto& buf = gltf.buffers[bv.bufferIndex];
					std::visit(fastgltf::visitor{
						[](const auto&) {},
						[&](const fastgltf::sources::Vector& vec) {
							pixels = stbi_load_from_memory(vec.bytes.data() + bv.byteOffset, static_cast<int>(bv.byteLength), &w, &h, &ch, STBI_rgb_alpha);
						},
						[&](const fastgltf::sources::ByteView& bvBytes) {
							pixels = stbi_load_from_memory(reinterpret_cast<const uint8_t*>(bvBytes.bytes.data()) + bv.byteOffset, static_cast<int>(bv.byteLength), &w, &h, &ch, STBI_rgb_alpha);
						}
					}, buf.data);
				}
				}, image.data);

			if (pixels) {
				uint64_t byteSize = static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 4ULL;
				CookedTextureHeader texHeader{
					.magic = GTEX_MAGIC,
					.version = 1,
					.width = static_cast<uint32_t>(w),
					.height = static_cast<uint32_t>(h),
					.depth = 1,
					.mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(w, h)))) + 1u,
					.arrayLayers = 1,
					.format = format,
					.magFilter = magFilter,
					.minFilter = minFilter,
					.mipmapMode = mipmapMode,
					.addressModeU = addressU,
					.addressModeV = addressV,
					.payloadSizeBytes = byteSize
				};
				write_gtex_file(cookedTexPath, texHeader, std::span<const uint8_t>(pixels, byteSize));
				stbi_image_free(pixels);
			}
			else {
				fmt::println("AssetImporter: failed to decode image {} for model '{}'", i, modelMeta.name);
			}
		}
	}

	// export each gltf material as editable .mat YAML
	std::vector<UUID> materialUUIDs(gltf.materials.size(), UUID::null());
	std::filesystem::path materialsDir = filePath.parent_path() / "materials";

	for (size_t i = 0; i < gltf.materials.size(); i++) {
		const fastgltf::Material& gltfMat = gltf.materials[i];
		std::string matName = gltfMat.name.empty() ? fmt::format("{}_mat_{}", modelMeta.name, i) : fmt::format("{}_{}", modelMeta.name, std::string(gltfMat.name));

		UUID matUUID = AssetRegistry::get().register_sub_asset(modelMeta.uuid, matName, AssetType::Material);
		materialUUIDs[i] = matUUID;

		MaterialAssetData matData{};
		matData.uuid = matUUID;
		matData.name = matName;
		matData.templateId = "pbr_default_template";
		matData.doubleSided = gltfMat.doubleSided;

		matData.colorFactors = glm::vec4(
			gltfMat.pbrData.baseColorFactor[0],
			gltfMat.pbrData.baseColorFactor[1],
			gltfMat.pbrData.baseColorFactor[2],
			gltfMat.pbrData.baseColorFactor[3]
		);

		float transmissionFactor = (gltfMat.transmission != nullptr) ? gltfMat.transmission->transmissionFactor : 0.0f;
		matData.transmissionFactor = transmissionFactor;

		if (gltfMat.alphaMode == fastgltf::AlphaMode::Blend || transmissionFactor > 0.0f) {
			matData.alphaMode = AlphaMode::Blend;
			matData.passType = MaterialPassType::Transparent;

			// adjust alpha based on transmission factor if base color alpha is fully opaque
			if (transmissionFactor > 0.0f && matData.colorFactors.a >= 0.99f) {
				matData.colorFactors.a = glm::clamp(1.0f - transmissionFactor * 0.75f, 0.15f, 0.85f);
			}
		}
		else if (gltfMat.alphaMode == fastgltf::AlphaMode::Mask) {
			matData.alphaMode = AlphaMode::Mask;
			matData.passType = MaterialPassType::Opaque;
		}
		else {
			matData.alphaMode = AlphaMode::Opaque;
			matData.passType = MaterialPassType::Opaque;
		}

		matData.metalRoughFactors = glm::vec4(gltfMat.pbrData.metallicFactor, gltfMat.pbrData.roughnessFactor, 0.0f, 0.0f);
		matData.occlusionStrength = gltfMat.occlusionTexture.has_value() ? gltfMat.occlusionTexture->strength : 1.0f;
		matData.normalScale = gltfMat.normalTexture.has_value() ? gltfMat.normalTexture->scale : 1.0f;
		matData.emissiveFactor = glm::vec3(gltfMat.emissiveFactor[0], gltfMat.emissiveFactor[1], gltfMat.emissiveFactor[2]);
		matData.emissiveStrength = 1.0f;

		auto resolve_gltf_tex_uuid = [&](size_t texIndex) -> UUID {
			if (texIndex < gltf.textures.size() && gltf.textures[texIndex].imageIndex.has_value()) {
				size_t imgIdx = gltf.textures[texIndex].imageIndex.value();
				if (imgIdx < imageUUIDs.size())
					return imageUUIDs[imgIdx];
			}
			return UUID::null();
			};

		if (gltfMat.pbrData.baseColorTexture.has_value())
			matData.albedoTextureUUID = resolve_gltf_tex_uuid(gltfMat.pbrData.baseColorTexture->textureIndex);

		if (gltfMat.normalTexture.has_value())
			matData.normalTextureUUID = resolve_gltf_tex_uuid(gltfMat.normalTexture->textureIndex);

		if (gltfMat.pbrData.metallicRoughnessTexture.has_value())
			matData.metalRoughTextureUUID = resolve_gltf_tex_uuid(gltfMat.pbrData.metallicRoughnessTexture->textureIndex);

		if (gltfMat.occlusionTexture.has_value())
			matData.occlusionTextureUUID = resolve_gltf_tex_uuid(gltfMat.occlusionTexture->textureIndex);

		if (gltfMat.emissiveTexture.has_value())
			matData.emissiveTextureUUID = resolve_gltf_tex_uuid(gltfMat.emissiveTexture->textureIndex);

		// save to .mat file (overwrite if missing or if existing .mat has stale model/texture UUIDs)
		std::filesystem::path matFilePath = materialsDir / (matName + ".mat");
		bool shouldSaveMat = !std::filesystem::exists(matFilePath);
		if (!shouldSaveMat) {
			MaterialAssetData existingMat{};
			if (!AssetSerializer::load_material_file(matFilePath, existingMat) || existingMat.uuid != matUUID) {
				shouldSaveMat = true;
			}
			else {
				auto is_slot_stale = [](const UUID& existingUuid, const UUID& expectedUuid) {
					if (!expectedUuid.is_valid()) return false;
					if (!existingUuid.is_valid()) return true;
					std::filesystem::path gtex = std::filesystem::path(".cache/textures") / (existingUuid.to_string() + ".gtex");
					return !std::filesystem::exists(gtex) && !AssetRegistry::get().contains(existingUuid);
					};
				if (is_slot_stale(existingMat.albedoTextureUUID, matData.albedoTextureUUID) ||
					is_slot_stale(existingMat.normalTextureUUID, matData.normalTextureUUID) ||
					is_slot_stale(existingMat.metalRoughTextureUUID, matData.metalRoughTextureUUID) ||
					is_slot_stale(existingMat.occlusionTextureUUID, matData.occlusionTextureUUID) ||
					is_slot_stale(existingMat.emissiveTextureUUID, matData.emissiveTextureUUID)) {
					shouldSaveMat = true;
				}
			}
		}
		if (shouldSaveMat) {
			AssetSerializer::save_material_file(matFilePath, matData);
		}
		if (AssetMetadata* subMeta = AssetRegistry::get().get_metadata_mutable(matUUID)) {
			subMeta->virtualPath = matFilePath;
			subMeta->path = matFilePath;
			subMeta->cookedPath = matFilePath;
		}
	}

	// cook each gltf mesh into .gmesh with lods
	std::vector<std::vector<PrefabSubmeshBinding>> cookedMeshBindings(gltf.meshes.size());
	for (size_t mIdx = 0; mIdx < gltf.meshes.size(); mIdx++) {
		const fastgltf::Mesh& mesh = gltf.meshes[mIdx];

		for (size_t pIdx = 0; pIdx < mesh.primitives.size(); pIdx++) {
			const fastgltf::Primitive& prim = mesh.primitives[pIdx];
			auto posIt = prim.findAttribute("POSITION");
			if (posIt == prim.attributes.end())
				continue;

			std::string subMeshName = fmt::format("{}_mesh_{}_prim_{}", modelMeta.name, mIdx, pIdx);
			UUID subMeshUUID = AssetRegistry::get().register_sub_asset(modelMeta.uuid, subMeshName, AssetType::Mesh);
			std::filesystem::path cookedMeshPath = std::filesystem::path(".cache/meshes") / (subMeshUUID.to_string() + ".gmesh");

			// extract vertices
			const fastgltf::Accessor& posAccessor = gltf.accessors[posIt->second];
			std::vector<Vertex> vertices(posAccessor.count);
			fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, posAccessor, [&](glm::vec3 v, size_t idx) {
				vertices[idx].position = v;
				vertices[idx].normal = glm::vec3(0.0f, 1.0f, 0.0f);
				vertices[idx].color = glm::vec4(1.0f);
				vertices[idx].uv_x = 0.0f;
				vertices[idx].uv_y = 0.0f;
				vertices[idx].tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
				});

			if (auto normIt = prim.findAttribute("NORMAL"); normIt != prim.attributes.end()) {
				fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, gltf.accessors[normIt->second], [&](glm::vec3 n, size_t idx) {
					vertices[idx].normal = n;
					});
			}

			bool hasUVs = false;
			if (auto uvIt = prim.findAttribute("TEXCOORD_0"); uvIt != prim.attributes.end()) {
				hasUVs = true;
				fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf, gltf.accessors[uvIt->second], [&](glm::vec2 uv, size_t idx) {
					vertices[idx].uv_x = uv.x;
					vertices[idx].uv_y = uv.y;
					});
			}

			if (auto colorIt = prim.findAttribute("COLOR_0"); colorIt != prim.attributes.end()) {
				const fastgltf::Accessor& acc = gltf.accessors[colorIt->second];
				if (acc.type == fastgltf::AccessorType::Vec3) {
					fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, acc, [&](glm::vec3 c, size_t idx) {
						vertices[idx].color = glm::vec4(c, 1.0f);
						});
				}
				else if (acc.type == fastgltf::AccessorType::Vec4) {
					fastgltf::iterateAccessorWithIndex<glm::vec4>(gltf, acc, [&](glm::vec4 c, size_t idx) {
						vertices[idx].color = c;
						});
				}
			}

			// extract indices
			std::vector<uint32_t> indices;
			if (prim.indicesAccessor.has_value()) {
				const fastgltf::Accessor& idxAcc = gltf.accessors[prim.indicesAccessor.value()];
				indices.reserve(idxAcc.count);
				fastgltf::iterateAccessor<uint32_t>(gltf, idxAcc, [&](uint32_t idx) {
					indices.push_back(idx);
					});
			}
			else {
				indices.resize(vertices.size());
				for (uint32_t v = 0; v < static_cast<uint32_t>(vertices.size()); ++v) {
					indices[v] = v;
				}
			}

			if (auto tanIt = prim.findAttribute("TANGENT"); tanIt != prim.attributes.end()) {
				fastgltf::iterateAccessorWithIndex<glm::vec4>(gltf, gltf.accessors[tanIt->second], [&](glm::vec4 t, size_t idx) {
					vertices[idx].tangent = t;
					});
			}
			else if (hasUVs && !indices.empty()) {
				compute_missing_tangents(vertices, indices);
			}

			Bounds submeshBounds{};
			cook_mesh_data(cookedMeshPath, vertices, indices, modelMeta.meshSettings, submeshBounds);

			PrefabSubmeshBinding binding{};
			binding.meshAssetUUID = subMeshUUID;
			binding.materialAssetUUID = (prim.materialIndex.has_value() && prim.materialIndex.value() < materialUUIDs.size()) ?
				materialUUIDs[prim.materialIndex.value()] : UUID::null();
			binding.startIndex = 0;
			binding.indexCount = static_cast<uint32_t>(indices.size());
			binding.bounds = submeshBounds;

			cookedMeshBindings[mIdx].push_back(binding);
		}
	}

	// export node hierarchy into .prefab yaml
	PrefabAssetData prefab{};
	prefab.uuid = AssetRegistry::get().register_sub_asset(modelMeta.uuid, modelMeta.name + "_prefab", AssetType::Prefab);
	prefab.name = modelMeta.name;
	prefab.sourceModelUUID = modelMeta.uuid;
	prefab.nodes.resize(gltf.nodes.size());

	for (size_t i = 0; i < gltf.nodes.size(); i++) {
		const fastgltf::Node& gltfNode = gltf.nodes[i];
		PrefabNodeData& outNode = prefab.nodes[i];
		outNode.name = gltfNode.name.empty() ? fmt::format("Node_{}", i) : std::string(gltfNode.name);

		std::visit(fastgltf::visitor{
			[&](const fastgltf::Node::TransformMatrix& matrix) {
				glm::mat4 mat(1.0f);
				std::memcpy(&mat, matrix.data(), sizeof(matrix));
				glm::vec3 scale;
				glm::vec3 translation;
				glm::vec3 skew;
				glm::vec4 perspective;
				glm::quat rotation;
				glm::decompose(mat, scale, rotation, translation, skew, perspective);
				outNode.position = translation;
				outNode.rotation = glm::degrees(glm::eulerAngles(rotation));
				outNode.scale = scale;
			},
			[&](const fastgltf::Node::TRS& trs) {
				outNode.position = glm::vec3(trs.translation[0], trs.translation[1], trs.translation[2]);
				glm::quat rot(trs.rotation[3], trs.rotation[0], trs.rotation[1], trs.rotation[2]);
				outNode.rotation = glm::degrees(glm::eulerAngles(rot));
				outNode.scale = glm::vec3(trs.scale[0], trs.scale[1], trs.scale[2]);
			}
			}, gltfNode.transform);

		// populate submesh bindings if node has a mesh
		if (gltfNode.meshIndex.has_value() && gltfNode.meshIndex.value() < cookedMeshBindings.size()) {
			outNode.submeshes = cookedMeshBindings[gltfNode.meshIndex.value()];
		}

		// populate children indices
		for (size_t childIdx : gltfNode.children) {
			outNode.childrenIndices.push_back(static_cast<uint32_t>(childIdx));
		}
	}

	// set parent indices for each node
	for (size_t i = 0; i < prefab.nodes.size(); i++) {
		for (uint32_t childIdx : prefab.nodes[i].childrenIndices) {
			if (childIdx < prefab.nodes.size()) {
				prefab.nodes[childIdx].parentIndex = static_cast<uint32_t>(i);
			}
		}
	}

	std::filesystem::path prefabPath = filePath;
	prefabPath.replace_extension(".prefab");
	AssetSerializer::save_prefab_file(prefabPath, prefab);

	fmt::println("AssetImporter: deconstructed '{}' -> {} textures, {} materials, {} meshes, '{}'",
		modelMeta.name, gltf.images.size(), gltf.materials.size(), gltf.meshes.size(), prefabPath.generic_string());

	return true;
}
