#include "resources/Mesh.h"

#include <graphics/VulkanCore.h>
#include <fmt/core.h>

Mesh::Mesh(const std::string& id, std::vector<uint32_t> indices, std::vector<Vertex> vertices, bool keepCpuGeometry)
	: Resource(id), _cpuIndices(indices), _cpuVertices(vertices) 
{
  if (keepCpuGeometry) {
    _pickingIndices = _cpuIndices;
    _pickingPositions.reserve(_cpuVertices.size());
    for (const auto& v : _cpuVertices) {
      _pickingPositions.push_back(v.position);
    }
  }
}

bool Mesh::doLoad(VulkanCore* core) {
  if (_cpuVertices.empty() || _cpuIndices.empty()) return false;

  _vertexCount = static_cast<uint32_t>(_cpuVertices.size());
  _indexCount = static_cast<uint32_t>(_cpuIndices.size());

  const size_t vertexBufferSize = _cpuVertices.size() * sizeof(Vertex);
  const size_t indexBufferSize = _cpuIndices.size() * sizeof(uint32_t);

	VkBufferUsageFlags vertexUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  _vertexBuffer = Buffer(get_id() + "_VB", std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(_cpuVertices.data()), vertexBufferSize), vertexUsage, VMA_MEMORY_USAGE_GPU_ONLY);
  _vertexBuffer.load(core);

	VkBufferUsageFlags indexUsage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  _indexBuffer = Buffer(get_id() + "_IB", std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(_cpuIndices.data()), indexBufferSize), indexUsage, VMA_MEMORY_USAGE_GPU_ONLY);
  _indexBuffer.load(core);

  _vertexBufferAddress = _vertexBuffer.get_device_address(core);

  // free CPU memory as it's now on the GPU
  _cpuVertices.clear();
  _cpuIndices.clear();

  return true;
}

bool Mesh::doUnload(VulkanCore* core) {
  _vertexBuffer.unload(core);
  _indexBuffer.unload(core);
  _vertexBufferAddress = 0;

  return true;
}