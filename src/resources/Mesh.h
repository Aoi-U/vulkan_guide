#pragma once

#include <graphics/utils/vk_types.h>
#include <resources/ResourceManager.h>
#include <resources/GlobalGeometryPool.h>
#include <resources/CookedFormats.h>
#include <resources/Buffer.h>
#include <cstdint>
#include <string>
#include <vector>
#include <array>

class Mesh : public Resource
{
public:
	// construct mesh from raw cpu vertex/index vectors
	explicit Mesh(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, const std::vector<uint32_t>&indices, const std::vector<Vertex>& vertices, bool keepCpuGeometry = true);

	// construct mesh from pre-allocated geometry in the global geometry pool
	explicit Mesh(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, const GeometryAllocation& alloc, std::vector<glm::vec3> pickingPositions, std::vector<uint32_t> pickingIndices);

	// construct mesh from cooked .gmesh binary file
	explicit Mesh(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, const std::filesystem::path& cookedPath);

	~Mesh() override { 
		fmt::println("Mesh: unloading mesh {}", get_id());
		Resource::unload(); 
	}

	VkBuffer get_vertex_buffer() const { return _allocation.vertexBuffer; }
	VkBuffer get_index_buffer() const { return _resourceManager->get_geometry_pool().get_index_buffer(); }
	VkDeviceAddress get_vertex_buffer_address() const { return _allocation.vertexBufferAddress; }
	uint32_t get_index_count(uint32_t lod = 0) const 
	{ 
		uint32_t clamped = (lod < _lodCount) ? lod : 0;
		return (_lodCount > 0) ? _lods[clamped].indexCount : _allocation.indexCount;
	}
	uint32_t get_first_index(uint32_t lod = 0) const 
	{ 
		uint32_t clamped = (lod < _lodCount) ? lod : 0;
		return _allocation.firstIndex + ((_lodCount > 0) ? _lods[clamped].indexOffset : 0);
	}

	uint32_t get_vertex_count() const { return _allocation.vertexCount; }
	uint32_t get_lod_count() const { return _lodCount; }
	const std::array<CookedMeshLODInfo, MAX_MESH_LODS>& get_lods() const { return _lods; }
	const Bounds& get_bounds() const { return _bounds; }

	std::span<const glm::vec3> get_picking_positions() const { return _pickingPositions; }
	std::span<const uint32_t> get_picking_indices() const { return _pickingIndices; }
	bool has_picking_data() const { return !_pickingPositions.empty() && !_pickingIndices.empty(); }

protected:
	bool doLoad() override;
	bool doUnload() override;

private:
	bool load_from_cooked_file();
	
	std::filesystem::path _cookedPath;
	std::vector<uint32_t> _cpuIndices;
	std::vector<Vertex> _cpuVertices;

	GeometryAllocation _allocation{};
	Bounds _bounds{};
	uint32_t _lodCount{ 1 };
	std::array<CookedMeshLODInfo, MAX_MESH_LODS> _lods{};

	std::vector<glm::vec3> _pickingPositions;
	std::vector<uint32_t> _pickingIndices;
};