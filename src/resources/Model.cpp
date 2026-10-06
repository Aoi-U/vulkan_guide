#include "Model.h"

#include <graphics/VulkanCore.h>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/parser.hpp>
#include <fastgltf/tools.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <iostream>
#include <limits>

Model::Model(const std::string& id, const std::filesystem::path& path, ResourceManager* manager)
	: Resource(id), _filePath(path), _resourceManager(manager)
{}

bool Model::doLoad(VulkanCore* core)
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

	// load textures
	for (size_t i = 0; i < gltf.images.size(); i++) {
		fastgltf::Image& image = gltf.images[i];
		std::string texName = get_id() + "_tex_" + std::to_string(i);

		std::visit(fastgltf::visitor{
			[](auto& arg) {},
			[&](fastgltf::sources::URI& filePath) { // case: textures stored outside the gltf/glb file
				std::filesystem::path p = _filePath.parent_path() / std::string(filePath.uri.path().begin(), filePath.uri.path().end());
				_resourceManager->load<Texture>(texName, p);
			},
			[&](fastgltf::sources::Vector& vector) { // case: textures stored inside the gltf/glb file
				_resourceManager->load<Texture>(texName, std::span<const uint8_t>(vector.bytes.data(), vector.bytes.size()));
			},
			[&](fastgltf::sources::BufferView& view) { // case: textures stored inside the gltf/glb file as a buffer view
				auto& bufferView = gltf.bufferViews[view.bufferViewIndex];
				auto& buffer = gltf.buffers[bufferView.bufferIndex];
				std::visit(fastgltf::visitor{
					[](auto& arg) {},
					[&](fastgltf::sources::Vector& vector) {
						_resourceManager->load<Texture>(texName, std::span<const uint8_t>(vector.bytes.data() + bufferView.byteOffset, bufferView.byteLength));
					},
					[&](fastgltf::sources::ByteView& byteView) {
						_resourceManager->load<Texture>(texName, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(byteView.bytes.data()) + bufferView.byteOffset, bufferView.byteLength));
					}
				}, buffer.data);
			}
		}, image.data);
	}

	// load materials

	for (size_t i = 0; i < gltf.materials.size(); i++) {
		fastgltf::Material& mat = gltf.materials[i];
		std::string matName = get_id() + "_mat_" + std::to_string(i);

		MaterialPassType passType = mat.alphaMode == fastgltf::AlphaMode::Blend ? MaterialPassType::Transparent : MaterialPassType::Opaque;
		auto matHandle = _resourceManager->load<Material>(matName, _resourceManager, passType); // get default material handle 

		MaterialConstants constants;
		constants.colorFactors.x = mat.pbrData.baseColorFactor[0];
		constants.colorFactors.y = mat.pbrData.baseColorFactor[1];
		constants.colorFactors.z = mat.pbrData.baseColorFactor[2];
		constants.colorFactors.w = mat.pbrData.baseColorFactor[3];

		constants.metalRoughFactors.x = mat.pbrData.metallicFactor;
		constants.metalRoughFactors.y = mat.pbrData.roughnessFactor;
		constants.metalRoughFactors.z = 0.0f;
		constants.metalRoughFactors.w = 0.0f;

		matHandle->set_constants(constants);

		// grab textures from gltf file
		if (mat.pbrData.baseColorTexture.has_value()) { // base color texture
			size_t imgIdx = gltf.textures[mat.pbrData.baseColorTexture->textureIndex].imageIndex.value(); 
			matHandle->set_texture(Material::TextureSlot::BaseColor, ResourceHandle<Texture>(get_id() + "_tex_" + std::to_string(imgIdx), _resourceManager)); 
		}

		if (mat.pbrData.metallicRoughnessTexture.has_value()) { // metallic roughness texture
			size_t imgIdx = gltf.textures[mat.pbrData.metallicRoughnessTexture->textureIndex].imageIndex.value(); 
			matHandle->set_texture(Material::TextureSlot::MetallicRoughness, ResourceHandle<Texture>(get_id() + "_tex_" + std::to_string(imgIdx), _resourceManager));
		}

		if (mat.occlusionTexture.has_value()) { // occlusion texture
			size_t imgIdx = gltf.textures[mat.occlusionTexture->textureIndex].imageIndex.value(); 
			matHandle->set_texture(Material::TextureSlot::Occlusion, ResourceHandle<Texture>(get_id() + "_tex_" + std::to_string(imgIdx), _resourceManager));
		}

		matHandle->update_buffer(core);
	}

	// load default material if no materials were found
	auto defaultMat = _resourceManager->load<Material>(get_id() + "_defaultMat", _resourceManager, MaterialPassType::Opaque);

	// load meshes
	std::vector<uint32_t> indices;
	std::vector<Vertex> vertices;
	std::vector<std::vector<ModelSubMesh>> meshSubmeshes(gltf.meshes.size());
	for (size_t i = 0; i < gltf.meshes.size(); i++) {
		fastgltf::Mesh& mesh = gltf.meshes[i];
		
		for (size_t p = 0; p < mesh.primitives.size(); p++) {
			fastgltf::Primitive& prim = mesh.primitives[p];
			indices.clear();
			vertices.clear();

			// load indices
			if (prim.indicesAccessor.has_value()) {
				fastgltf::Accessor& accessor = gltf.accessors[prim.indicesAccessor.value()];
				indices.reserve(accessor.count);
				fastgltf::iterateAccessor<uint32_t>(gltf, accessor, [&](uint32_t idx) {indices.push_back(idx); });
			}

			// load vertices
			auto posIt = prim.findAttribute("POSITION");
			if (posIt != prim.attributes.end()) {
				fastgltf::Accessor& accessor = gltf.accessors[posIt->second];
				vertices.resize(accessor.count);
				fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, accessor, [&](glm::vec3 v, size_t idx) {
					vertices[idx].position = v;
					vertices[idx].normal = glm::vec3(1, 0, 0);
					vertices[idx].color = glm::vec4(1.0);
					vertices[idx].uv_x = 0;
					vertices[idx].uv_y = 0;
					});
			}

			// load normals
			auto normIt = prim.findAttribute("NORMAL");
			if (normIt != prim.attributes.end()) {
				fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, gltf.accessors[normIt->second], [&](glm::vec3 v, size_t idx) {
					vertices[idx].normal = v;
					});
			}

			// load uvs
			auto uvIt = prim.findAttribute("TEXCOORD_0");
			if (uvIt != prim.attributes.end()) {
				fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf, gltf.accessors[uvIt->second], [&](glm::vec2 v, size_t idx) {
					vertices[idx].uv_x = v.x;
					vertices[idx].uv_y = v.y;
					});
			}

			// calculate bounds
			glm::vec3 minPos(std::numeric_limits<int>::max()), maxPos(std::numeric_limits<int>::min());
			for (const Vertex& v : vertices) {
				minPos = glm::min(minPos, v.position);
				maxPos = glm::max(maxPos, v.position);
			}

			ModelSubMesh sm;
			sm.startIndex = 0;
			sm.count = static_cast<uint32_t>(indices.empty() ? vertices.size() : indices.size());
			sm.bounds.origin = (maxPos + minPos) / 2.0f;
			sm.bounds.extents = (maxPos - minPos) / 2.0f;
			sm.bounds.sphereRadius = glm::length(sm.bounds.extents);

			// create a mesh resource for the submesh
			std::string meshName = get_id() + "_mesh_" + std::to_string(i) + "_prim_" + std::to_string(p);
			sm.mesh = _resourceManager->load<Mesh>(meshName, std::move(indices), std::move(vertices));

			// assign material to the submesh, defaulting to a fallback material if none is specified
			if (prim.materialIndex.has_value()) {
				sm.material = ResourceHandle<Material>(get_id() + "_mat_" + std::to_string(prim.materialIndex.value()), _resourceManager);
			}
			else {
				sm.material = defaultMat;
			}

			// store submesh
			meshSubmeshes[i].push_back(sm);
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

bool Model::doUnload(VulkanCore* core)
{
	rootNodes.clear();
	allNodes.clear();

	return true;
}
