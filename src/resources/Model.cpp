#include "Model.h"

#include <graphics/VulkanCore.h>
#include <graphics/utils/vk_utils.h>
#include <resources/DefaultResources.h>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/parser.hpp>
#include <fastgltf/tools.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <stb_image.h>
#include <iostream>
#include <limits>

static VkFilter extractFilter(fastgltf::Filter filter) {
	switch (filter) {
	case fastgltf::Filter::Nearest:
	case fastgltf::Filter::NearestMipMapNearest:
	case fastgltf::Filter::NearestMipMapLinear:
		return VK_FILTER_NEAREST;
	default:
		return VK_FILTER_LINEAR;
	}
}

static VkSamplerMipmapMode extractMipmap(fastgltf::Filter filter) {
	switch (filter) {
	case fastgltf::Filter::NearestMipMapNearest:
	case fastgltf::Filter::LinearMipMapNearest:
		return VK_SAMPLER_MIPMAP_MODE_NEAREST;
	default:
		return VK_SAMPLER_MIPMAP_MODE_LINEAR;
	}
}

static VkSamplerAddressMode extractWrap(fastgltf::Wrap wrap) {
	switch (wrap) {
	case fastgltf::Wrap::ClampToEdge:
		return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	case fastgltf::Wrap::MirroredRepeat:
		return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
	default:
		return VK_SAMPLER_ADDRESS_MODE_REPEAT;
	}
}


Model::Model(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, const std::filesystem::path& path)
	: Resource(id, core, resourceManager), _filePath(path)
{}

void Model::wait_until_ready() const
{
	while (!_isReady.load(std::memory_order_acquire)) {
		// prevent deadlocks if main thread gets blocked while waiting
		_resourceManager->process_main_thread_tasks();
		std::this_thread::yield();
	}
}

bool Model::doLoad()
{
	_aborted.store(false, std::memory_order_release);
	_isReady.store(false, std::memory_order_release);

	// start worker thread to load model data asynchronously
	_workerFuture = std::async(std::launch::async, [this]() {
		auto token = std::make_shared<ModelStagingToken>(); // holds intermediate data during loading

		if (parse_gltf_file(token->gltf)) {
			if (extract_textures(token->gltf, token->rawImages)) {
				extract_geometry(token->gltf, token->uploadContext);

				// pack loaded data into token and queue finalization on main thread
				_resourceManager->queue_main_thread_task([this, t = token]() mutable {
					if (!this->_aborted.load(std::memory_order_acquire)) {
						this->finalize_upload(std::move(t));
					}
					});
				return;
			}
			fmt::println("Model: failed to extract textures from model '{}'", _filePath.string());
		}
		fmt::println("Model: failed to parse glTF file '{}'", _filePath.string());
		this->_aborted.store(true, std::memory_order_release); // mark as aborted if any step fails
		this->_isReady.store(true, std::memory_order_release);
		});

	return true;
}

bool Model::doUnload()
{
	// signal worker thread to abort if still running
	_aborted.store(true, std::memory_order_release);
	if (_workerFuture.valid()) { // wait for worker thread to finish if still running
		_workerFuture.wait();
	}

	rootNodes.clear();
	allNodes.clear();
	_materials.clear();
	_textures.clear();

	_isReady.store(false, std::memory_order_release);

	return true;
}

bool Model::parse_gltf_file(fastgltf::Asset& gltf)
{
	fastgltf::Parser parser{ 
		fastgltf::Extensions::KHR_materials_transmission | 
		fastgltf::Extensions::KHR_materials_ior
	};
	constexpr auto gltfOptions =
		fastgltf::Options::DontRequireValidAssetMember |
		fastgltf::Options::AllowDouble |
		fastgltf::Options::LoadGLBBuffers |
		fastgltf::Options::LoadExternalBuffers;

	fastgltf::GltfDataBuffer data;
	data.loadFromFile(_filePath);
	auto type = fastgltf::determineGltfFileType(&data);

	if (type == fastgltf::GltfType::glTF) { // load gltf file
		auto load = parser.loadGLTF(&data, _filePath.parent_path(), gltfOptions);
		if (load)
			gltf = std::move(load.get());
		else
			return false;
	}
	else if (type == fastgltf::GltfType::GLB) { // load glb file
		auto load = parser.loadBinaryGLTF(&data, _filePath.parent_path(), gltfOptions);
		if (load)
			gltf = std::move(load.get());
		else
			return false;
	}
	else { // invalid file type
		return false;
	}

	return true;
}

bool Model::extract_textures(const fastgltf::Asset& gltf, std::vector<RawImageData>& rawImages)
{
	rawImages.resize(gltf.images.size());
	std::vector<bool> isSRGB(gltf.textures.size(), false);

	// determine srgb textures
	for (const fastgltf::Material& mat : gltf.materials) {
		if (mat.pbrData.baseColorTexture.has_value())
			isSRGB[mat.pbrData.baseColorTexture.value().textureIndex] = true;

		if (mat.emissiveTexture.has_value())
			isSRGB[mat.emissiveTexture.value().textureIndex] = true;
	}

	// determine texture formats 
	for (size_t i = 0; i < gltf.images.size(); i++) {
		const fastgltf::Image& image = gltf.images[i];

		for (size_t t = 0; t < gltf.textures.size(); t++) {
			if (gltf.textures[t].imageIndex.value_or(-1) == i) {
				if (isSRGB[t]) {
					rawImages[i].format = VK_FORMAT_R8G8B8A8_SRGB;
				}
				if (gltf.textures[t].samplerIndex.has_value()) {
					const fastgltf::Sampler& sampler = gltf.samplers[gltf.textures[t].samplerIndex.value()];
					rawImages[i].magFilter = sampler.magFilter.has_value() ? extractFilter(sampler.magFilter.value()) : VK_FILTER_LINEAR;
					rawImages[i].minFilter = sampler.minFilter.has_value() ? extractFilter(sampler.minFilter.value()) : VK_FILTER_LINEAR;
					rawImages[i].mipmapMode = sampler.minFilter.has_value() ? extractMipmap(sampler.minFilter.value()) : VK_SAMPLER_MIPMAP_MODE_LINEAR;
					rawImages[i].addressModeU = extractWrap(sampler.wrapS);
					rawImages[i].addressModeV = extractWrap(sampler.wrapT);
				}
				break;
			}
		}
	}

	// load textures
	std::vector<std::future<void>> futures;
	for (size_t i = 0; i < gltf.images.size(); i++) {
		// load each image in separate thread
		futures.push_back(std::async(std::launch::async, [&gltf, this, &rawImages, i]() {
			const fastgltf::Image& image = gltf.images[i];
			std::visit(fastgltf::visitor{
				[](auto& arg) {},
				[&](const fastgltf::sources::URI& filePath) {
					std::filesystem::path p = _filePath.parent_path() / std::string(filePath.uri.path().begin(), filePath.uri.path().end());
					rawImages[i].pixels = stbi_load(p.string().c_str(), &rawImages[i].width, &rawImages[i].height, &rawImages[i].channels, STBI_rgb_alpha);
				},
				[&](const fastgltf::sources::Vector& vector) {
					rawImages[i].pixels = stbi_load_from_memory(vector.bytes.data(), static_cast<int>(vector.bytes.size()), &rawImages[i].width, &rawImages[i].height, &rawImages[i].channels, STBI_rgb_alpha);
				},
				[&](const fastgltf::sources::BufferView& view) {
					auto& bufferView = gltf.bufferViews[view.bufferViewIndex];
					auto& buffer = gltf.buffers[bufferView.bufferIndex];
					std::visit(fastgltf::visitor{
						[](auto& arg) {},
						[&](const fastgltf::sources::Vector& vector) {
							rawImages[i].pixels = stbi_load_from_memory(vector.bytes.data() + bufferView.byteOffset, static_cast<int>(bufferView.byteLength), &rawImages[i].width, &rawImages[i].height, &rawImages[i].channels, STBI_rgb_alpha);
						},
						[&](const fastgltf::sources::ByteView& byteView) {
							rawImages[i].pixels = stbi_load_from_memory(reinterpret_cast<const uint8_t*>(byteView.bytes.data()) + bufferView.byteOffset, static_cast<int>(bufferView.byteLength), &rawImages[i].width, &rawImages[i].height, &rawImages[i].channels, STBI_rgb_alpha);
						}
					}, buffer.data);
				}
				}, image.data);
			})
		);
	}

	// wait for all threads to finish loading
	for (auto& future : futures) {
		future.get();
	}

	return true;
}

void Model::extract_geometry(const fastgltf::Asset& gltf, ModelStagingUploadContext& uploadContext)
{
	uploadContext.meshSubmeshes.resize(gltf.meshes.size());
	uploadContext.meshPickingPositions.resize(gltf.meshes.size());
	uploadContext.meshPickingIndices.resize(gltf.meshes.size());

	for (size_t i = 0; i < gltf.meshes.size(); i++) {
		const fastgltf::Mesh& mesh = gltf.meshes[i];

		size_t meshIndexStart = uploadContext.indices.size();
		size_t meshVertexStart = uploadContext.vertices.size();

		for (size_t p = 0; p < mesh.primitives.size(); p++) {
			const fastgltf::Primitive& prim = mesh.primitives[p];
			ModelSubMesh sm;

			// record index relative to global index buffer
			sm.startIndex = static_cast<uint32_t>(uploadContext.indices.size() - meshIndexStart);
			size_t initialVertex = uploadContext.vertices.size();

			size_t localVertexOffset = initialVertex - meshVertexStart;

			std::vector<uint32_t> localIndices;

			if (prim.indicesAccessor.has_value()) { // load indices
				const fastgltf::Accessor& accessor = gltf.accessors[prim.indicesAccessor.value()];
				sm.count = static_cast<uint32_t>(accessor.count);

				uploadContext.indices.reserve(uploadContext.indices.size() + accessor.count);
				fastgltf::iterateAccessor<uint32_t>(gltf, accessor, [&](uint32_t idx) {
					uint32_t localIdx = idx + static_cast<uint32_t>(localVertexOffset);
					uint32_t globalIdx = idx + static_cast<uint32_t>(initialVertex);

					uploadContext.indices.push_back(localIdx); // local index for GPU 
					localIndices.push_back(globalIdx);         // global index for tangent calc
					uploadContext.meshPickingIndices[i].push_back(localIdx);
					});
			}
			else {
				// generate sequential indices if none exist
				const fastgltf::Accessor& posAccessor = gltf.accessors[prim.findAttribute("POSITION")->second];
				sm.count = static_cast<uint32_t>(posAccessor.count);
				uploadContext.indices.reserve(uploadContext.indices.size() + sm.count);
				for (uint32_t v = 0; v < sm.count; v++) {
					uint32_t localIdx = v + static_cast<uint32_t>(localVertexOffset);
					uint32_t globalIdx = v + static_cast<uint32_t>(initialVertex);

					uploadContext.indices.push_back(localIdx);
					localIndices.push_back(globalIdx);
					uploadContext.meshPickingIndices[i].push_back(localIdx);
				}
			}

			// load vertices
			auto posIt = prim.findAttribute("POSITION");
			if (posIt != prim.attributes.end()) {
				const fastgltf::Accessor& accessor = gltf.accessors[posIt->second];
				uploadContext.vertices.resize(initialVertex + accessor.count);
				fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, accessor, [&](glm::vec3 v, size_t idx) {
					uploadContext.vertices[idx + initialVertex].position = v;
					uploadContext.vertices[idx + initialVertex].normal = glm::vec3(1, 0, 0);
					uploadContext.vertices[idx + initialVertex].color = glm::vec4(1.0f);
					uploadContext.vertices[idx + initialVertex].uv_x = 0;
					uploadContext.vertices[idx + initialVertex].uv_y = 0;
					uploadContext.vertices[idx + initialVertex].tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
					uploadContext.meshPickingPositions[i].push_back(v);
					});
			}

			// load normals
			auto normIt = prim.findAttribute("NORMAL");
			if (normIt != prim.attributes.end()) {
				fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, gltf.accessors[normIt->second], [&](glm::vec3 v, size_t idx) {
					uploadContext.vertices[idx + initialVertex].normal = v;
					});
			}

			// load uvs
			auto uvIt = prim.findAttribute("TEXCOORD_0");
			if (uvIt != prim.attributes.end()) {
				fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf, gltf.accessors[uvIt->second], [&](glm::vec2 v, size_t idx) {
					uploadContext.vertices[idx + initialVertex].uv_x = v.x;
					uploadContext.vertices[idx + initialVertex].uv_y = v.y;
					});
			}

			// load colors
			auto colorIt = prim.findAttribute("COLOR_0");
			if (colorIt != prim.attributes.end()) {
				const fastgltf::Accessor& accessor = gltf.accessors[colorIt->second];
				if (accessor.type == fastgltf::AccessorType::Vec3) {
					fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, accessor, [&](glm::vec3 c, size_t idx) {
						uploadContext.vertices[idx + initialVertex].color = glm::vec4(c, 1.0f);
						});
				}
				else if (accessor.type == fastgltf::AccessorType::Vec4) {
					fastgltf::iterateAccessorWithIndex<glm::vec4>(gltf, accessor, [&](glm::vec4 c, size_t idx) {
						uploadContext.vertices[idx + initialVertex].color = c;
						});
				}
			}

			// load tangents
			auto tangentIt = prim.findAttribute("TANGENT");
			if (tangentIt != prim.attributes.end()) {
				fastgltf::iterateAccessorWithIndex<glm::vec4>(gltf, gltf.accessors[tangentIt->second], [&](glm::vec4 v, size_t idx) {
					uploadContext.vertices[idx + initialVertex].tangent = v;
					});
			}
			else if (uvIt != prim.attributes.end() && !localIndices.empty()) { // no tangents provided, calculate them from positions and uvs
				for (size_t j = 0; j + 2 < localIndices.size(); j += 3) {
					Vertex& v0 = uploadContext.vertices[localIndices[j + 0]];
					Vertex& v1 = uploadContext.vertices[localIndices[j + 1]];
					Vertex& v2 = uploadContext.vertices[localIndices[j + 2]];

					glm::vec3 edge1 = v1.position - v0.position;
					glm::vec3 edge2 = v2.position - v0.position;
					glm::vec2 deltaUV1 = glm::vec2(v1.uv_x, v1.uv_y) - glm::vec2(v0.uv_x, v0.uv_y);
					glm::vec2 deltaUV2 = glm::vec2(v2.uv_x, v2.uv_y) - glm::vec2(v0.uv_x, v0.uv_y);

					float det = (deltaUV1.x * deltaUV2.y - deltaUV2.x * deltaUV1.y);
					glm::vec3 tangent = glm::vec3(1.0f, 0.0f, 0.0f);
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
				for (size_t k = initialVertex; k < uploadContext.vertices.size(); k++) {
					Vertex& v = uploadContext.vertices[k];
					glm::vec3 t = glm::vec3(v.tangent);
					glm::vec3 n = v.normal;
					float NdotT = glm::dot(n, t);
					glm::vec3 orthoDir = t - n * NdotT;
					glm::vec3 ortho = glm::length(orthoDir) > 1e-6f ? glm::normalize(orthoDir) : glm::vec3(1.0f, 0.0f, 0.0f);
					v.tangent = glm::vec4(ortho, 1.0f);
				}
			}

			// calculate bounds
			glm::vec3 minPos = uploadContext.vertices[initialVertex].position;
			glm::vec3 maxPos = uploadContext.vertices[initialVertex].position;
			for (size_t iIdx = initialVertex; iIdx < uploadContext.vertices.size(); iIdx++) {
				minPos = glm::min(minPos, uploadContext.vertices[iIdx].position);
				maxPos = glm::max(maxPos, uploadContext.vertices[iIdx].position);
			}

			sm.bounds.origin = (maxPos + minPos) / 2.0f;
			sm.bounds.extents = (maxPos - minPos) / 2.0f;
			sm.bounds.sphereRadius = glm::length(sm.bounds.extents);

			// temporarily store gltf material index in bounding struct to resolve later
			sm.material = std::make_shared<MaterialInstance>(DefaultResources::get().defaultPBRTemplate);
			if (prim.materialIndex.has_value()) {
				sm.bounds.sphereRadius = prim.materialIndex.value() * -1.0f - 1000.0f; // store material index in sphere radius for later assignment
			}
			uploadContext.meshSubmeshes[i].push_back(sm);
		}
	}
}

void Model::finalize_upload(std::shared_ptr<ModelStagingToken> token)
{
	upload_gpu_resources(token->gltf, token->rawImages, token->uploadContext);
	create_materials(token->gltf, token->uploadContext);
	create_meshes(token->gltf, token->uploadContext);
	build_scene_hierarchy(token->gltf, token->uploadContext);

	_isReady.store(true, std::memory_order_release);
}

void Model::upload_gpu_resources(const fastgltf::Asset& gltf, std::vector<RawImageData>& rawImages, ModelStagingUploadContext& uploadContext)
{
	if (uploadContext.vertices.empty() || uploadContext.indices.empty()) // nothing to upload
		return;

	bool meshesAlreadyLoaded = !uploadContext.meshSubmeshes.empty();
	for (size_t i = 0; i < uploadContext.meshSubmeshes.size(); ++i) {
		std::string meshName = get_id() + "_mesh_" + std::to_string(i);
		if (!_resourceManager->has_resource<Mesh>(meshName)) {
			meshesAlreadyLoaded = false;
			break;
		}
	}

	// identify which textures need uploading
	_textures.resize(gltf.images.size());
	std::vector<bool> textureNeedsUpload(rawImages.size(), false);
	size_t totalTextureStagingSize = 0;

	for (size_t i = 0; i < rawImages.size(); ++i) {
		if (!rawImages[i].pixels) continue;

		std::string texName = get_id() + "_tex_" + std::to_string(i);
		if (_resourceManager->has_resource<Texture>(texName)) { // texture exists, reuse handle and release raw CPU memory 
			_textures[i] = _resourceManager->get_handle<Texture>(texName);
			stbi_image_free(rawImages[i].pixels);
			rawImages[i].pixels = nullptr;
		}
		else {
			textureNeedsUpload[i] = true;
			totalTextureStagingSize += rawImages[i].width * rawImages[i].height * 4;
		}
	}

	// if both geometry and textures already exist, skip all GPU allocations
	if (meshesAlreadyLoaded && totalTextureStagingSize == 0) {
		return;
	}

	size_t vertSize = meshesAlreadyLoaded ? 0 : uploadContext.vertices.size() * sizeof(Vertex);
	size_t indSize = meshesAlreadyLoaded ? 0 : uploadContext.indices.size() * sizeof(uint32_t);

	if (!meshesAlreadyLoaded) {
		uploadContext.geomAlloc = _resourceManager->get_geometry_pool().allocate(
			static_cast<uint32_t>(uploadContext.vertices.size()),
			static_cast<uint32_t>(uploadContext.indices.size())
		);
	}

	size_t totalStagingSize = vertSize + indSize + totalTextureStagingSize;
	if (totalStagingSize == 0)
		return;

	ResourceHandle<Buffer> staging = _resourceManager->load<Buffer>(get_id() + "_batch_staging", totalStagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	uint8_t* mapped = static_cast<uint8_t*>(staging->get_mapped_data());
	size_t offset = 0;

	if (!meshesAlreadyLoaded) {
		memcpy(mapped, uploadContext.vertices.data(), vertSize); // copy all vertex data to staging
		offset += vertSize;
		memcpy(mapped + offset, uploadContext.indices.data(), indSize); // copy all index data to staging
		offset += indSize;
	}

	std::vector<size_t> imgOffsets(rawImages.size());
	for (size_t i = 0; i < rawImages.size(); i++) {
		if (!textureNeedsUpload[i] || !rawImages[i].pixels)
			continue;

		size_t imgSize = rawImages[i].width * rawImages[i].height * 4;

		memcpy(mapped + offset, rawImages[i].pixels, imgSize); // copy this image data to staging
		imgOffsets[i] = offset;
		offset += imgSize;

		stbi_image_free(rawImages[i].pixels);
		rawImages[i].pixels = nullptr;

		TextureDescription desc{
			.extent = { static_cast<uint32_t>(rawImages[i].width), static_cast<uint32_t>(rawImages[i].height), 1},
			.format = rawImages[i].format,
			.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			.mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(desc.extent.width, desc.extent.height)))) + 1,
			.magFilter = rawImages[i].magFilter,
			.minFilter = rawImages[i].minFilter,
			.mipmapMode = rawImages[i].mipmapMode,
			.addressModeU = rawImages[i].addressModeU,
			.addressModeV = rawImages[i].addressModeV,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.deferBindlessRegistration = true // batch register all textures later
		};

		std::string texName = get_id() + "_tex_" + std::to_string(i);
		_textures[i] = _resourceManager->load<Texture>(texName, desc);
	}

	_core->immediate_submit([&](VkCommandBuffer cmd) {
		// copy all vertex data to gpu
		VkBufferCopy vCopy{
			.srcOffset = 0,
			.dstOffset = uploadContext.geomAlloc.vertexOffset * sizeof(Vertex),
			.size = vertSize
		};
		vkCmdCopyBuffer(cmd, staging->get_buffer(), _resourceManager->get_geometry_pool().get_vertex_buffer(), 1, &vCopy);

		// copy all index data to gpu
		VkBufferCopy iCopy{
			.srcOffset = vertSize,
			.dstOffset = uploadContext.geomAlloc.firstIndex * sizeof(uint32_t),
			.size = indSize
		};
		vkCmdCopyBuffer(cmd, staging->get_buffer(), _resourceManager->get_geometry_pool().get_index_buffer(), 1, &iCopy);

		// copy all textures to gpu
		for (size_t i = 0; i < rawImages.size(); i++) {
			if (!_textures[i].is_valid())
				continue;

			VkImage image = _textures[i]->get_image();
			vkutil::image_mem_barrier2(cmd, image, rawImages[i].format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, _textures[i]->get_mip_levels(), 0);

			VkBufferImageCopy imgCopy{};
			imgCopy.bufferOffset = imgOffsets[i];
			imgCopy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			imgCopy.imageSubresource.layerCount = 1;
			imgCopy.imageExtent = _textures[i]->get_extent();
			vkCmdCopyBufferToImage(cmd, staging->get_buffer(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &imgCopy);

			vkutil::generate_mipmaps(cmd, image, { imgCopy.imageExtent.width, imgCopy.imageExtent.height });
		}
		});

	// batch register bindless descriptors
	std::vector<std::pair<VkImageView, VkSampler>> batchTexData;
	std::vector<size_t> validIndices;
	for (size_t i = 0; i < _textures.size(); i++) {
		if (textureNeedsUpload[i] && _textures[i].is_valid()) {
			batchTexData.push_back({ _textures[i]->get_image_view(), _textures[i]->get_sampler() });
			validIndices.push_back(i);
		}
	}

	if (!batchTexData.empty()) {
		std::vector<uint32_t> outSlots;
		_resourceManager->register_bindless_texture_batched(batchTexData, outSlots);
		for (size_t i = 0; i < validIndices.size(); ++i) {
			_textures[validIndices[i]]->set_bindless_index(outSlots[i]);
		}
	}

	staging->unload();
}

void Model::create_materials(const fastgltf::Asset& gltf, const ModelStagingUploadContext& uploadContext)
{
	_materials.resize(gltf.materials.size());
	ResourceHandle<MaterialTemplate> pbrTemplate = DefaultResources::get().defaultPBRTemplate;

	for (size_t i = 0; i < gltf.materials.size(); i++) {
		const fastgltf::Material& gltfMat = gltf.materials[i];
		auto matInstance = std::make_shared<MaterialInstance>(pbrTemplate);

		// base color factor
		glm::vec4 baseColorFactor = glm::vec4(
			gltfMat.pbrData.baseColorFactor[0],
			gltfMat.pbrData.baseColorFactor[1],
			gltfMat.pbrData.baseColorFactor[2],
			gltfMat.pbrData.baseColorFactor[3]
		);
		matInstance->set_parameter("colorFactors", baseColorFactor);

		float transmissionFactor = 0.0f;
		if (gltfMat.transmission != nullptr) {
			transmissionFactor = gltfMat.transmission->transmissionFactor;
		}

		if (gltfMat.alphaMode == fastgltf::AlphaMode::Blend || transmissionFactor > 0.0f) {
			matInstance->set_alpha_mode(AlphaMode::Blend);

			// Adjust alpha based on transmission factor if base color alpha is fully opaque
			if (transmissionFactor > 0.0f && baseColorFactor.a >= 0.99f) {
				baseColorFactor.a = glm::clamp(1.0f - transmissionFactor * 0.75f, 0.15f, 0.85f);
			}
		}
		else if (gltfMat.alphaMode == fastgltf::AlphaMode::Mask) {
			matInstance->set_alpha_mode(AlphaMode::Mask);
		}
		else {
			matInstance->set_alpha_mode(AlphaMode::Opaque);
		}

		matInstance->set_double_sided(gltfMat.doubleSided);
		matInstance->set_parameter("colorFactors", baseColorFactor);

		// metallic roughness factor
		glm::vec4 metalRoughFactor = glm::vec4(
			gltfMat.pbrData.metallicFactor,
			gltfMat.pbrData.roughnessFactor,
			0.0f,
			0.0f
		);
		matInstance->set_parameter("metalRoughFactors", metalRoughFactor);

		// extra factors
		glm::vec4 extraFactors = glm::vec4(
			gltfMat.occlusionTexture.has_value() ? gltfMat.occlusionTexture->strength : 1.0f,
			gltfMat.normalTexture.has_value() ? gltfMat.normalTexture->scale : 1.0f,
			gltfMat.doubleSided ? 1.0f : 0.0f,
			transmissionFactor
		);
		matInstance->set_parameter("extraFactors", extraFactors);

		// emissive factor
		glm::vec4 emissiveFactor = glm::vec4(
			gltfMat.emissiveFactor[0], gltfMat.emissiveFactor[1], gltfMat.emissiveFactor[2], 1.0f
		);
		matInstance->set_parameter("emissiveFactor", emissiveFactor);

		// base color texture
		if (gltfMat.pbrData.baseColorTexture.has_value()) {
			size_t imgIdx = gltf.textures[gltfMat.pbrData.baseColorTexture->textureIndex].imageIndex.value();
			matInstance->set_texture("albedo", _textures[imgIdx]);
		}
		
		// metallic roughness texture
		if (gltfMat.pbrData.metallicRoughnessTexture.has_value()) {
			size_t imgIdx = gltf.textures[gltfMat.pbrData.metallicRoughnessTexture->textureIndex].imageIndex.value();
			matInstance->set_texture("metalRough", _textures[imgIdx]);
		}

		// occlusion texture
		if (gltfMat.occlusionTexture.has_value()) {
			size_t imgIdx = gltf.textures[gltfMat.occlusionTexture->textureIndex].imageIndex.value();
			matInstance->set_texture("occlusion", _textures[imgIdx]);
		}

		// normal texture
		if (gltfMat.normalTexture.has_value()) {
			size_t imgIdx = gltf.textures[gltfMat.normalTexture->textureIndex].imageIndex.value();
			matInstance->set_texture("normal", _textures[imgIdx]);
		}

		// emissive texture
		if (gltfMat.emissiveTexture.has_value()) {
			size_t imgIdx = gltf.textures[gltfMat.emissiveTexture->textureIndex].imageIndex.value();
			matInstance->set_texture("emissive", _textures[imgIdx]);
		}

		_materials[i] = matInstance;
	}
}

void Model::create_meshes(const fastgltf::Asset& gltf, const ModelStagingUploadContext& uploadContext)
{
	auto defaultMat = std::make_shared<MaterialInstance>(DefaultResources::get().defaultPBRTemplate);

	uint32_t currentFirstIndex = uploadContext.geomAlloc.firstIndex;
	uint32_t currentVertexOffset = uploadContext.geomAlloc.vertexOffset;
	VkDeviceAddress currentVertexAddress = uploadContext.geomAlloc.vertexBufferAddress;

	// create logical Handles mapping the suballocations
	for (size_t i = 0; i < uploadContext.meshSubmeshes.size(); i++) {
		std::string meshName = get_id() + "_mesh_" + std::to_string(i);
		ResourceHandle<Mesh> batchedMesh;

		uint32_t indexCount = static_cast<uint32_t>(uploadContext.meshPickingIndices[i].size());
		uint32_t vertexCount = static_cast<uint32_t>(uploadContext.meshPickingPositions[i].size());

		GeometryAllocation meshAlloc;
		meshAlloc.firstIndex = currentFirstIndex;
		meshAlloc.indexCount = indexCount;
		meshAlloc.vertexOffset = currentVertexOffset;
		meshAlloc.vertexCount = vertexCount;
		meshAlloc.vertexBufferAddress = currentVertexAddress;

		if (_resourceManager->has_resource<Mesh>(meshName)) {
			batchedMesh = _resourceManager->get_handle<Mesh>(meshName);
		}
		else {
			batchedMesh = _resourceManager->load<Mesh>(
				meshName,
				meshAlloc,
				uploadContext.meshPickingPositions[i],
				uploadContext.meshPickingIndices[i]
			);
		}

		currentFirstIndex += indexCount;
		currentVertexOffset += vertexCount;
		currentVertexAddress += vertexCount * sizeof(Vertex);

		// copy out and restore real values from the extract pass
		auto& smList = const_cast<std::vector<ModelSubMesh>&>(uploadContext.meshSubmeshes[i]);
		for (auto& sm : smList) {
			sm.mesh = batchedMesh;
			// restore material and original radius
			if (sm.bounds.sphereRadius <= -1000.0f) {
				int matIndex = static_cast<int>((sm.bounds.sphereRadius + 1000.0f) * -1.0f);
				sm.material = _materials[matIndex];
			}
			else {
				sm.material = defaultMat;
			}
			sm.bounds.sphereRadius = glm::length(sm.bounds.extents);
		}
	}
}

void Model::build_scene_hierarchy(const fastgltf::Asset& gltf, const ModelStagingUploadContext& uploadContext)
{
	// craete model nodes from gltf scene graph
	for (size_t i = 0; i < gltf.nodes.size(); i++) {
		const fastgltf::Node& node = gltf.nodes[i];
		auto modelNode = std::make_shared<ModelNode>();

		std::visit(fastgltf::visitor{
			[&](const fastgltf::Node::TransformMatrix& matrix) {
				memcpy(&modelNode->localTransform, matrix.data(), sizeof(matrix));
			},
			[&](const fastgltf::Node::TRS& transform) {
				glm::vec3 tl(transform.translation[0], transform.translation[1], transform.translation[2]);
				glm::quat rot(transform.rotation[3], transform.rotation[0], transform.rotation[1], transform.rotation[2]);
				glm::vec3 sc(transform.scale[0], transform.scale[1], transform.scale[2]);
				modelNode->localTransform = glm::translate(glm::mat4(1.0f), tl) * glm::toMat4(rot) * glm::scale(glm::mat4(1.0f), sc);
			}
			}, node.transform);

		if (node.meshIndex.has_value()) {
			modelNode->submeshes = uploadContext.meshSubmeshes[node.meshIndex.value()];
		}
		allNodes.push_back(modelNode);
	}

	// loop again to setup transform hierarchy
	for (size_t i = 0; i < gltf.nodes.size(); i++) {
		for (auto& c : gltf.nodes[i].children) {
			allNodes[i]->children.push_back(allNodes[c]);
			allNodes[c]->parent = allNodes[i];
		}
	}

	// find all top nodes
	for (auto& node : allNodes) {
		if (node->parent.lock() == nullptr) {
			rootNodes.push_back(std::dynamic_pointer_cast<ModelNode>(node));
			node->refreshTransform(glm::mat4(1.0f));
		}
	}
}
