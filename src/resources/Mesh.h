#pragma once

#include <core/vk_types.h>
#include <cstdint>
#include <filesystem>
#include <resources/ResourceManager.h>
#include <string>
#include <vector>

class Mesh : public Resource
{
public:
	explicit Mesh(const std::string& id, std::vector<uint32_t> indices, std::vector<Vertex> vertices);
	~Mesh() override = default;

	VkBuffer get_vertex_buffer() const { return _vertexBuffer.buffer; }
	VkBuffer get_index_buffer() const { return _indexBuffer.buffer; }
	VkDeviceAddress get_vertex_buffer_address() const { return _vertexBufferAddress; }
	uint32_t get_index_count() const { return _indexCount; }
	uint32_t get_vertex_count() const { return _vertexCount; }

protected:
	bool doLoad(VulkanCore* core) override;
	bool doUnload(VulkanCore* core) override;

private:
	std::vector<uint32_t> _cpuIndices;
	std::vector<Vertex> _cpuVertices;

	AllocatedBuffer _vertexBuffer{};
	AllocatedBuffer _indexBuffer{};
	VkDeviceAddress _vertexBufferAddress{ 0 };
	uint32_t _indexCount{ 0 };
	uint32_t _vertexCount{ 0 };
};