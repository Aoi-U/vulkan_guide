#pragma once

#include <graphics/utils/vk_types.h>
#include <cstdint>
#include <filesystem>
#include <resources/ResourceManager.h>
#include <resources/GlobalGeometryPool.h>
#include <resources/Buffer.h>
#include <string>
#include <vector>

class Mesh : public Resource
{
public:
	explicit Mesh(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, std::vector<uint32_t> indices, std::vector<Vertex> vertices, bool keepCpuGeometry = true);

	// construct mesh from pre-allocated geometry in the global geometry pool
	explicit Mesh(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, GeometryAllocation alloc, std::vector<glm::vec3> pickingPositions, std::vector<uint32_t> pickingIndices);

	~Mesh() override { 
		fmt::println("Mesh: unloading mesh {}", get_id());
		Resource::unload(); }

	VkBuffer get_vertex_buffer() const { return _resourceManager->get_geometry_pool().get_vertex_buffer(); }
	VkBuffer get_index_buffer() const { return _resourceManager->get_geometry_pool().get_index_buffer(); }
	VkDeviceAddress get_vertex_buffer_address() const { return _allocation.vertexBufferAddress; }
	uint32_t get_index_count() const { return _allocation.indexCount; }
	uint32_t get_vertex_count() const { return _allocation.vertexCount; }
	uint32_t get_first_index() const { return _allocation.firstIndex; }

	std::span<const glm::vec3> get_picking_positions() const { return _pickingPositions; }
	std::span<const uint32_t> get_picking_indices() const { return _pickingIndices; }
	bool has_picking_data() const { return !_pickingPositions.empty() && !_pickingIndices.empty(); }

protected:
	bool doLoad() override;
	bool doUnload() override;

private:
	std::vector<uint32_t> _cpuIndices;
	std::vector<Vertex> _cpuVertices;

	GeometryAllocation _allocation{};

	std::vector<glm::vec3> _pickingPositions;
	std::vector<uint32_t> _pickingIndices;
};