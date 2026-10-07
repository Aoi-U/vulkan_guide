#pragma once

#include <core/vk_types.h>
#include <cstdint>
#include <filesystem>
#include <resources/ResourceManager.h>
#include <resources/Buffer.h>
#include <string>
#include <vector>

class Mesh : public Resource
{
public:
	explicit Mesh(const std::string& id, std::vector<uint32_t> indices, std::vector<Vertex> vertices, bool keepCpuGeometry = true);
	~Mesh() override = default;

	VkBuffer get_vertex_buffer() const { return _vertexBuffer.get_buffer(); }
	VkBuffer get_index_buffer() const { return _indexBuffer.get_buffer(); }
	VkDeviceAddress get_vertex_buffer_address() const { return _vertexBufferAddress; }
	uint32_t get_index_count() const { return _indexCount; }
	uint32_t get_vertex_count() const { return _vertexCount; }

	std::span<const glm::vec3> get_picking_positions() const { return _pickingPositions; }
	std::span<const uint32_t> get_picking_indices() const { return _pickingIndices; }
	bool has_picking_data() const { return !_pickingPositions.empty() && !_pickingIndices.empty(); }

protected:
	bool doLoad(VulkanCore* core) override;
	bool doUnload(VulkanCore* core) override;

private:
	std::vector<uint32_t> _cpuIndices;
	std::vector<Vertex> _cpuVertices;

	Buffer _vertexBuffer{};
	Buffer _indexBuffer{};

	VkDeviceAddress _vertexBufferAddress{ 0 };
	uint32_t _indexCount{ 0 };
	uint32_t _vertexCount{ 0 };

	std::vector<glm::vec3> _pickingPositions;
	std::vector<uint32_t> _pickingIndices;
};