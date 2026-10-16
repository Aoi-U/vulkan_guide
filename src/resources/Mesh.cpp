#include "resources/Mesh.h"

#include <graphics/VulkanCore.h>
#include <graphics/utils/StagingUploader.h>
#include <fmt/core.h>

Mesh::Mesh(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, const std::vector<uint32_t>& indices, const std::vector<Vertex>& vertices, bool keepCpuGeometry)
	: Resource(id, core, resourceManager), _cpuIndices(indices), _cpuVertices(vertices)
{
	_lodCount = 1;
	_lods[0] = CookedMeshLODInfo{ 0, static_cast<uint32_t>(_cpuIndices.size()), 1.0f };

	if (keepCpuGeometry) {
		_pickingIndices = _cpuIndices;
		_pickingPositions.reserve(_cpuVertices.size());
		for (const auto& v : _cpuVertices) {
			_pickingPositions.push_back(v.position);
		}
	}
}

Mesh::Mesh(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, const GeometryAllocation& alloc, std::vector<glm::vec3> pickingPositions, std::vector<uint32_t> pickingIndices)
	: Resource(id, core, resourceManager), _pickingPositions(std::move(pickingPositions)), _pickingIndices(std::move(pickingIndices))
{
	_allocation = alloc;
	_lodCount = 1;
	_lods[0] = CookedMeshLODInfo{ 0, alloc.indexCount, 1.0f };
	load(); // mark as loaded so it skips doLoad() 
}

Mesh::Mesh(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, const std::filesystem::path& cookedPath)
	: Resource(id, core, resourceManager), _cookedPath(cookedPath)
{}

bool Mesh::doLoad() {
	if (_allocation.is_valid())
		return true;

	// if constructed with .gmesh path, read binary from .cache/meshes/
	if (!_cookedPath.empty() && _cpuVertices.empty()) {
		if (!load_from_cooked_file())
			return false;
	}

	if (_cpuVertices.empty() || _cpuIndices.empty())
		return false;

	// sub allocate space across all lods in paged globalgeometrypool
	_allocation = _resourceManager->get_geometry_pool().allocate(static_cast<uint32_t>(_cpuVertices.size()), static_cast<uint32_t>(_cpuIndices.size()));

	if (!_allocation.is_valid())
		return false;

	const size_t totalGpuBytes = _cpuVertices.size() * sizeof(Vertex) + _cpuIndices.size() * sizeof(uint32_t);
	const VkDeviceSize dstVertOffset = static_cast<VkDeviceSize>(_allocation.vertexOffset) * sizeof(Vertex);
	const VkDeviceSize dstIdxOffset = static_cast<VkDeviceSize>(_allocation.firstIndex) * sizeof(uint32_t);

	// stage transfer via stagingpuloader ring buffer
	UUID selfUuid = get_uuid();
	bool staged = _resourceManager->get_staging_uploader().stage_geometry_upload(
		_allocation.vertexBuffer,
		dstVertOffset,
		_cpuVertices,
		get_index_buffer(),
		dstIdxOffset,
		_cpuIndices,
		[selfUuid, totalGpuBytes]() { // callback to mark mesh as loaded
			if (selfUuid.is_valid()) {
				AssetRegistry::get().set_resource_state(selfUuid, ResourceState::Loaded);
				AssetRegistry::get().set_gpu_size(selfUuid, totalGpuBytes);
			}
		}
	);

	AssetRegistry::get().set_gpu_size(selfUuid, totalGpuBytes);
	_cpuVertices.clear();
	_cpuIndices.clear();
	
	return staged;
}

bool Mesh::doUnload() {
	// free mesh data from global geometry pool
	_resourceManager->get_geometry_pool().free(_allocation);
	_allocation = {};

	return true;
}

bool Mesh::load_from_cooked_file()
{
	std::ifstream in(_cookedPath, std::ios::binary);
	if (!in.is_open()) {
		fmt::println("Mesh: failed to open .gmesh file '{}'", _cookedPath.string());
		return false;
	}

	CookedMeshHeader header{};
	in.read(reinterpret_cast<char*>(&header), sizeof(CookedMeshHeader));
	if (!in.good() || header.magic != GMESH_MAGIC) {
		fmt::println("Mesh: invalid .gmesh header in '{}'", _cookedPath.string());
		return false;
	}

	_bounds = header.bounds;
	_lodCount = std::clamp(header.lodCount, 1u, MAX_MESH_LODS);
	for (uint32_t i = 0; i < MAX_MESH_LODS; i++) {
		_lods[i] = header.lods[i];
	}

	_cpuVertices.resize(header.vertexCount);
	_cpuIndices.resize(header.totalIndexCount);

	in.read(reinterpret_cast<char*>(_cpuVertices.data()), static_cast<std::streamsize>(header.vertexCount * sizeof(Vertex)));
	in.read(reinterpret_cast<char*>(_cpuIndices.data()), static_cast<std::streamsize>(header.totalIndexCount * sizeof(uint32_t)));

	if (header.hasPickingData) {
		_pickingPositions.resize(header.vertexCount);
		in.read(reinterpret_cast<char*>(_pickingPositions.data()), static_cast<std::streamsize>(header.vertexCount * sizeof(glm::vec3)));

		_pickingIndices.assign(_cpuIndices.begin(), _cpuIndices.begin() + _lods[0].indexCount);
	}

	return in.good();
}
