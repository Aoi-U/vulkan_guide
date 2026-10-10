#include "resources/Mesh.h"

#include <graphics/VulkanCore.h>
#include <fmt/core.h>

Mesh::Mesh(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, std::vector<uint32_t> indices, std::vector<Vertex> vertices, bool keepCpuGeometry)
	: Resource(id, core, resourceManager), _cpuIndices(indices), _cpuVertices(vertices)
{
  if (keepCpuGeometry) {
    _pickingIndices = _cpuIndices;
    _pickingPositions.reserve(_cpuVertices.size());
    for (const auto& v : _cpuVertices) {
      _pickingPositions.push_back(v.position);
    }
  }
}

Mesh::Mesh(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, GeometryAllocation alloc, std::vector<glm::vec3> pickingPositions, std::vector<uint32_t> pickingIndices)
  : Resource(id, core, resourceManager), _pickingPositions(std::move(pickingPositions)), _pickingIndices(std::move(pickingIndices))
{
  _allocation = alloc;
  load(); // mark as loaded so it skips doLoad() 
}

bool Mesh::doLoad() {
  if (_allocation.vertexBufferAddress != 0)
    return true;

  if (_cpuVertices.empty() || _cpuIndices.empty()) return false;

  // allocate space in the global geometry pool for this meshs vertices and indices
	_allocation = _resourceManager->get_geometry_pool().allocate(
    static_cast<uint32_t>(_cpuVertices.size()), 
    static_cast<uint32_t>(_cpuIndices.size())
  );

  const size_t vertexBufferSize = _cpuVertices.size() * sizeof(Vertex);
  const size_t indexBufferSize = _cpuIndices.size() * sizeof(uint32_t);

	// create staging buffer to transfer vertex and index data to gpu
  size_t stagingSize = vertexBufferSize + indexBufferSize;
  ResourceHandle<Buffer> staging = _resourceManager->load<Buffer>(get_id() + "_stagin", stagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);

  // copy vertex and index data into staging 
  uint8_t* mapped = static_cast<uint8_t*>(staging->get_mapped_data());
  memcpy(mapped, _cpuVertices.data(), vertexBufferSize);
  memcpy(mapped + vertexBufferSize, _cpuIndices.data(), indexBufferSize);

  // submit command buffer to copy from staging to global buffers
  _core->immediate_submit([&](VkCommandBuffer cmd) {
    VkBufferCopy vertCopy{};
    vertCopy.srcOffset = 0;
    vertCopy.dstOffset = _allocation.vertexOffset * sizeof(Vertex);
    vertCopy.size = vertexBufferSize;
		vkCmdCopyBuffer(cmd, staging->get_buffer(), get_vertex_buffer(), 1, &vertCopy);

    VkBufferCopy idxCopy{};
    idxCopy.srcOffset = vertexBufferSize;
		idxCopy.dstOffset = _allocation.firstIndex * sizeof(uint32_t);
    idxCopy.size = indexBufferSize;
    vkCmdCopyBuffer(cmd, staging->get_buffer(), get_index_buffer(), 1, &idxCopy);
    });
  // unload staging
  staging->unload();

  _cpuVertices.clear();
  _cpuIndices.clear();

  return true;
}

bool Mesh::doUnload() {
	// free mesh data from global geometry pool
  _resourceManager->get_geometry_pool().free(_allocation);

  return true;
}