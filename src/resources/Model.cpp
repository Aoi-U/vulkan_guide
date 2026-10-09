#include "Model.h"

#include <graphics/VulkanCore.h>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/parser.hpp>
#include <fastgltf/tools.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
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

bool Model::doLoad()
{
	fastgltf::Parser parser{};
	constexpr auto gltfOptions =
		fastgltf::Options::DontRequireValidAssetMember |
		fastgltf::Options::AllowDouble |
		fastgltf::Options::LoadGLBBuffers |
		fastgltf::Options::LoadExternalBuffers;

	fastgltf::GltfDataBuffer data;
	data.loadFromFile(_filePath);

	auto type = fastgltf::determineGltfFileType(&data);
	fastgltf::Asset gltf;
	if (type == fastgltf::GltfType::glTF) {
		auto load = parser.loadGLTF(&data, _filePath.parent_path(), gltfOptions);
		if (load)
			gltf = std::move(load.get());
		else {
			std::cerr << "Failed to load glTF: " << fastgltf::to_underlying(load.error()) << std::endl;
			return false;
		}
	}
	else if (type == fastgltf::GltfType::GLB) {
		auto load = parser.loadBinaryGLTF(&data, _filePath.parent_path(), gltfOptions);
		if (load)
			gltf = std::move(load.get());
		else {
			std::cerr << "Failed to load GLB: " << fastgltf::to_underlying(load.error()) << std::endl;
			return false;
		}
	}
	else {
		std::cerr << "Failed to determine glTF container" << std::endl;
		return false;
	}

	std::vector<bool> isSRGB(gltf.textures.size(), false);
	for (fastgltf::Material& mat : gltf.materials) {
		if (mat.pbrData.baseColorTexture.has_value()) {
			isSRGB[mat.pbrData.baseColorTexture.value().textureIndex] = true;
		}
		if (mat.emissiveTexture.has_value()) {
			isSRGB[mat.emissiveTexture.value().textureIndex] = true;
		}
	}

	// load textures
	std::vector<ResourceHandle<Texture>> textures(gltf.images.size());
	for (size_t i = 0; i < gltf.images.size(); i++) {
		fastgltf::Image& image = gltf.images[i];
		std::string texName = get_id() + "_tex_" + std::to_string(i);

		VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
		TextureDescription desc;
		for (size_t t = 0; t < gltf.textures.size(); t++) {
			if (gltf.textures[t].imageIndex.value_or(-1) == i) {
				if (isSRGB[t]) {
					format = VK_FORMAT_R8G8B8A8_SRGB;
				}
				if (gltf.textures[t].samplerIndex.has_value()) {
					fastgltf::Sampler& sampler = gltf.samplers[gltf.textures[t].samplerIndex.value()];
					desc.magFilter = sampler.magFilter.has_value() ? extractFilter(sampler.magFilter.value()) : VK_FILTER_LINEAR;
					desc.minFilter = sampler.minFilter.has_value() ? extractFilter(sampler.minFilter.value()) : VK_FILTER_LINEAR;
					desc.mipmapMode = sampler.minFilter.has_value() ? extractMipmap(sampler.minFilter.value()) : VK_SAMPLER_MIPMAP_MODE_LINEAR;
					desc.addressModeU = extractWrap(sampler.wrapS);
					desc.addressModeV = extractWrap(sampler.wrapT);
				}
				break;
			}
		}
		desc.format = format;
		desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
		desc.mipLevels = 0;

		std::visit(fastgltf::visitor{
			[](auto& arg) {},
			[&](fastgltf::sources::URI& filePath) { // case: textures stored outside the gltf/glb file
				std::filesystem::path p = _filePath.parent_path() / std::string(filePath.uri.path().begin(), filePath.uri.path().end());
				textures[i] = _resourceManager->load<Texture>(texName, p, format, desc.usage);
			},
			[&](fastgltf::sources::Vector& vector) { // case: textures stored inside the gltf/glb file
				textures[i] = _resourceManager->load<Texture>(texName, std::span<const uint8_t>(vector.bytes.data(), vector.bytes.size()), format);
			},
			[&](fastgltf::sources::BufferView& view) { // case: textures stored inside the gltf/glb file as a buffer view
				auto& bufferView = gltf.bufferViews[view.bufferViewIndex];
				auto& buffer = gltf.buffers[bufferView.bufferIndex];
				std::visit(fastgltf::visitor{
					[](auto& arg) {},
					[&](fastgltf::sources::Vector& vector) {
						textures[i] = _resourceManager->load<Texture>(texName, std::span<const uint8_t>(vector.bytes.data() + bufferView.byteOffset, bufferView.byteLength), format);
					},
					[&](fastgltf::sources::ByteView& byteView) {
						textures[i] = _resourceManager->load<Texture>(texName, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(byteView.bytes.data()) + bufferView.byteOffset, bufferView.byteLength), format);
					}
				}, buffer.data);
			}
			}, image.data);
	}
	_textures = textures;

	// load materials
	std::vector<ResourceHandle<Material>> materials(gltf.materials.size());
	for (size_t i = 0; i < gltf.materials.size(); i++) {
		fastgltf::Material& mat = gltf.materials[i];
		std::string matName = get_id() + "_mat_" + std::to_string(i);

		MaterialPassType passType = mat.alphaMode == fastgltf::AlphaMode::Blend ? MaterialPassType::Transparent : MaterialPassType::Opaque;
		materials[i] = _resourceManager->load<Material>(matName, passType); // get default material handle 

		MaterialConstants constants;
		constants.colorFactors.x = mat.pbrData.baseColorFactor[0];
		constants.colorFactors.y = mat.pbrData.baseColorFactor[1];
		constants.colorFactors.z = mat.pbrData.baseColorFactor[2];
		constants.colorFactors.w = mat.pbrData.baseColorFactor[3];

		constants.metalRoughFactors.x = mat.pbrData.metallicFactor;
		constants.metalRoughFactors.y = mat.pbrData.roughnessFactor;
		constants.metalRoughFactors.z = 0.0f;
		constants.metalRoughFactors.w = 0.0f;
		constants.extraFactors = glm::vec4(
			mat.occlusionTexture.has_value() ? mat.occlusionTexture->strength : 1.0f,
			mat.normalTexture.has_value() ? mat.normalTexture->scale : 1.0f,
			mat.doubleSided ? 1.0f : 0.0f,
			0.0f
		);
		constants.emissiveFactor.x = mat.emissiveFactor[0];
		constants.emissiveFactor.y = mat.emissiveFactor[1];
		constants.emissiveFactor.z = mat.emissiveFactor[2];
		constants.emissiveFactor.w = 1.0f;

		materials[i]->set_constants(constants);

		// grab textures from gltf file
		if (mat.pbrData.baseColorTexture.has_value()) { // base color texture
			size_t imgIdx = gltf.textures[mat.pbrData.baseColorTexture->textureIndex].imageIndex.value();
			materials[i]->set_texture(Material::TextureSlot::BaseColor, ResourceHandle<Texture>(get_id() + "_tex_" + std::to_string(imgIdx), _resourceManager));
		}

		if (mat.pbrData.metallicRoughnessTexture.has_value()) { // metallic roughness texture
			size_t imgIdx = gltf.textures[mat.pbrData.metallicRoughnessTexture->textureIndex].imageIndex.value();
			materials[i]->set_texture(Material::TextureSlot::MetallicRoughness, ResourceHandle<Texture>(get_id() + "_tex_" + std::to_string(imgIdx), _resourceManager));
		}

		if (mat.occlusionTexture.has_value()) { // occlusion texture
			size_t imgIdx = gltf.textures[mat.occlusionTexture->textureIndex].imageIndex.value();
			materials[i]->set_texture(Material::TextureSlot::Occlusion, ResourceHandle<Texture>(get_id() + "_tex_" + std::to_string(imgIdx), _resourceManager));
		}

		if (mat.normalTexture.has_value()) { // normal texture
			size_t imgIdx = gltf.textures[mat.normalTexture->textureIndex].imageIndex.value();
			materials[i]->set_texture(Material::TextureSlot::Normal, ResourceHandle<Texture>(get_id() + "_tex_" + std::to_string(imgIdx), _resourceManager));
		}

		if (mat.emissiveTexture.has_value()) { // emissive texture
			size_t imgIdx = gltf.textures[mat.emissiveTexture->textureIndex].imageIndex.value();
			materials[i]->set_texture(Material::TextureSlot::Emissive, ResourceHandle<Texture>(get_id() + "_tex_" + std::to_string(imgIdx), _resourceManager));
		}
	}
	_materials = materials;

	// load default material if no materials were found
	auto defaultMat = _resourceManager->load<Material>(get_id() + "_defaultMat", MaterialPassType::Opaque);

	// load meshes
	std::vector<uint32_t> indices;
	std::vector<Vertex> vertices;
	std::vector<std::vector<ModelSubMesh>> meshSubmeshes(gltf.meshes.size());
	for (size_t i = 0; i < gltf.meshes.size(); i++) {
		fastgltf::Mesh& mesh = gltf.meshes[i];

		indices.clear();
		vertices.clear();

		for (size_t p = 0; p < mesh.primitives.size(); p++) {
			fastgltf::Primitive& prim = mesh.primitives[p];

			ModelSubMesh sm;
			sm.startIndex = (uint32_t)indices.size();

			size_t initialVertex = vertices.size();

			// load indices
			if (prim.indicesAccessor.has_value()) {
				fastgltf::Accessor& accessor = gltf.accessors[prim.indicesAccessor.value()];
				sm.count = (uint32_t)accessor.count;

				indices.reserve(indices.size() + accessor.count);
				fastgltf::iterateAccessor<uint32_t>(gltf, accessor, [&](uint32_t idx) {
					indices.push_back(idx + initialVertex);
					});
			}
			else {
				// generate sequential indices if none exist
				fastgltf::Accessor& posAccessor = gltf.accessors[prim.findAttribute("POSITION")->second];
				sm.count = (uint32_t)posAccessor.count;
				indices.reserve(indices.size() + sm.count);
				for (uint32_t v = 0; v < sm.count; v++) {
					indices.push_back(initialVertex + v);
				}
			}

			// load vertices
			auto posIt = prim.findAttribute("POSITION");
			if (posIt != prim.attributes.end()) {
				fastgltf::Accessor& accessor = gltf.accessors[posIt->second];
				vertices.resize(initialVertex + accessor.count);

				fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, accessor, [&](glm::vec3 v, size_t idx) {
					vertices[idx + initialVertex].position = v;
					vertices[idx + initialVertex].normal = glm::vec3(1, 0, 0);
					vertices[idx + initialVertex].color = glm::vec4(1.0);
					vertices[idx + initialVertex].uv_x = 0;
					vertices[idx + initialVertex].uv_y = 0;
					vertices[idx + initialVertex].tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
					});
			}

			// load normals
			auto normIt = prim.findAttribute("NORMAL");
			if (normIt != prim.attributes.end()) {
				fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, gltf.accessors[normIt->second], [&](glm::vec3 v, size_t idx) {
					vertices[idx + initialVertex].normal = v;
					});
			}

			// load uvs
			auto uvIt = prim.findAttribute("TEXCOORD_0");
			if (uvIt != prim.attributes.end()) {
				fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf, gltf.accessors[uvIt->second], [&](glm::vec2 v, size_t idx) {
					vertices[idx + initialVertex].uv_x = v.x;
					vertices[idx + initialVertex].uv_y = v.y;
					});
			}

			// load colors
			auto colorIt = prim.findAttribute("COLOR_0");
			if (colorIt != prim.attributes.end()) {
				fastgltf::Accessor& accessor = gltf.accessors[colorIt->second];
				if (accessor.type == fastgltf::AccessorType::Vec3) {
					fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, accessor, [&](glm::vec3 c, size_t idx) {
						vertices[idx + initialVertex].color = glm::vec4(c, 1.0f);
						});
				}
				else if (accessor.type == fastgltf::AccessorType::Vec4) {
					fastgltf::iterateAccessorWithIndex<glm::vec4>(gltf, accessor, [&](glm::vec4 c, size_t idx) {
						vertices[idx + initialVertex].color = c;
						});
				}
			}

			// load tangents
			auto tangentIt = prim.findAttribute("TANGENT");
			std::vector<uint32_t> tempIndices = std::vector<uint32_t>(indices.begin() + sm.startIndex, indices.end());

			if (tangentIt != prim.attributes.end()) {
				fastgltf::iterateAccessorWithIndex<glm::vec4>(gltf, gltf.accessors[tangentIt->second], [&](glm::vec4 v, size_t idx) {
					vertices[idx + initialVertex].tangent = v;
					});
			}
			else if (uvIt != prim.attributes.end() && !tempIndices.empty()) { // no tangents provided, calculate them from positions and uvs
				for (size_t j = 0; j + 2 < tempIndices.size(); j += 3) {
					Vertex& v0 = vertices[tempIndices[j + 0]];
					Vertex& v1 = vertices[tempIndices[j + 1]];
					Vertex& v2 = vertices[tempIndices[j + 2]];

					glm::vec3 edge1 = v1.position - v0.position;
					glm::vec3 edge2 = v2.position - v0.position;
					glm::vec2 deltaUV1 = glm::vec2(v1.uv_x, v1.uv_y) - glm::vec2(v0.uv_x, v0.uv_y);
					glm::vec2 deltaUV2 = glm::vec2(v2.uv_x, v2.uv_y) - glm::vec2(v0.uv_x, v0.uv_y);

					float det = (deltaUV1.x * deltaUV2.y - deltaUV2.x * deltaUV1.y);
					glm::vec3 tangent = glm::vec3(1.0f, 0.0f, 0.0f);
					if (std::abs(det) > 1e-6f) {
						float f = 1.0 / det;
						tangent.x = f * (deltaUV2.y * edge1.x - deltaUV1.y * edge2.x);
						tangent.y = f * (deltaUV2.y * edge1.y - deltaUV1.y * edge2.y);
						tangent.z = f * (deltaUV2.y * edge1.z - deltaUV1.y * edge2.z);
					}

					v0.tangent += glm::vec4(tangent, 0.0f);
					v1.tangent += glm::vec4(tangent, 0.0f);
					v2.tangent += glm::vec4(tangent, 0.0f);
				}
				for (int k = initialVertex; k < vertices.size(); k++) {
					Vertex& v = vertices[k];
					glm::vec3 t = glm::vec3(v.tangent);
					glm::vec3 n = v.normal;
					float NdotT = glm::dot(n, t);

					glm::vec3 orthoDir = t - n * NdotT;
					glm::vec3 ortho = glm::length(orthoDir) > 1e-6f ? glm::normalize(orthoDir) : glm::vec3(1.0f, 0.0f, 0.0f);
					v.tangent = glm::vec4(ortho, 1.0f);
				}
			}

			// calculate bounds
			glm::vec3 minPos = vertices[initialVertex].position;
			glm::vec3 maxPos = vertices[initialVertex].position;
			for (int i = initialVertex; i < vertices.size(); i++) {
				minPos = glm::min(minPos, vertices[i].position);
				maxPos = glm::max(maxPos, vertices[i].position);
			}

			sm.bounds.origin = (maxPos + minPos) / 2.0f;
			sm.bounds.extents = (maxPos - minPos) / 2.0f;
			sm.bounds.sphereRadius = glm::length(sm.bounds.extents);

			// assign material to the submesh, defaulting to a fallback material if none is specified
			if (prim.materialIndex.has_value()) {
				sm.material = _materials[prim.materialIndex.value()];
			}
			else {
				sm.material = defaultMat;
			}

			meshSubmeshes[i].push_back(sm);
		}

		std::string meshName = get_id() + "_mesh_" + std::to_string(i);
		auto batchedMesh = _resourceManager->load<Mesh>(meshName, indices, vertices);

		// assign the shared batched mesh to all primitives
		for (auto& sm : meshSubmeshes[i]) {
			sm.mesh = batchedMesh;
		}
	}

	// create model nodes from gltf scene graph
	for (size_t i = 0; i < gltf.nodes.size(); i++) {
		fastgltf::Node& node = gltf.nodes[i];
		auto modelNode = std::make_shared<ModelNode>();

		std::visit(fastgltf::visitor{
			[&](fastgltf::Node::TransformMatrix matrix) {
				memcpy(&modelNode->localTransform, matrix.data(), sizeof(matrix));
				},
			[&](fastgltf::Node::TRS transform) {
				glm::vec3 tl(transform.translation[0], transform.translation[1], transform.translation[2]);
				glm::quat rot(transform.rotation[3], transform.rotation[0], transform.rotation[1], transform.rotation[2]);
				glm::vec3 sc(transform.scale[0], transform.scale[1], transform.scale[2]);

				modelNode->localTransform = glm::translate(glm::mat4(1.0f), tl) * glm::toMat4(rot) * glm::scale(glm::mat4(1.0f), sc);
				}
			}, node.transform);

		if (node.meshIndex.has_value())
			modelNode->submeshes = meshSubmeshes[node.meshIndex.value()];

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


	return true;
}

bool Model::doUnload()
{
	rootNodes.clear();
	allNodes.clear();
	_materials.clear();
	_textures.clear();

	return true;
}
