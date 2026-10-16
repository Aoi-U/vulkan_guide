#include "resources/Mesh.h"

#include <graphics/VulkanCore.h>
#include <graphics/utils/StagingUploader.h>
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

  // suballocate from paged globalgeometrypool
	_allocation = _resourceManager->get_geometry_pool().allocate(
    static_cast<uint32_t>(_cpuVertices.size()), 
    static_cast<uint32_t>(_cpuIndices.size())
  );

  if (!_allocation.is_valid()) {
    return false;
  }

  // stage async transfer via StagingUploader ring buffer 
  const VkDeviceSize dstVertOffset = static_cast<VkDeviceSize>(_allocation.vertexOffset) * sizeof(Vertex);
  const VkDeviceSize dstIdxOffset = static_cast<VkDeviceSize>(_allocation.firstIndex) * sizeof(uint32_t);

	bool staged = _resourceManager->get_staging_uploader().stage_geometry_upload(
    _allocation.vertexBuffer,
    dstVertOffset,
    _cpuVertices,
    get_index_buffer(),
    dstIdxOffset,
    _cpuIndices
  );

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