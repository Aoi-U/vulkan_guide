#include "resources/Mesh.h"

#include <graphics/VulkanCore.h>
#include <fmt/core.h>

Mesh::Mesh(const std::string& id, std::vector<uint32_t> indices, std::vector<Vertex> vertices)
	: Resource(id), _cpuIndices(std::move(indices)), _cpuVertices(std::move(vertices)) 
{}

bool Mesh::doLoad(VulkanCore* core) {
  if (_cpuVertices.empty() || _cpuIndices.empty()) return false;

  _vertexCount = static_cast<uint32_t>(_cpuVertices.size());
  _indexCount = static_cast<uint32_t>(_cpuIndices.size());

  AllocatedMesh buffers = core->upload_mesh(_cpuIndices, _cpuVertices);

  // free CPU memory as it's now on the GPU
  _cpuVertices.clear();
  _cpuIndices.clear();

  _vertexBuffer = buffers.vertexBuffer;
  _indexBuffer = buffers.indexBuffer;
  _vertexBufferAddress = buffers.vertexBufferAddress;

  return true;
}

bool Mesh::doUnload(VulkanCore* core) {
  if (_vertexBuffer.buffer != VK_NULL_HANDLE) {
    core->destroy_buffer(_vertexBuffer);
    _vertexBuffer = {};
  }
  if (_indexBuffer.buffer != VK_NULL_HANDLE) {
    core->destroy_buffer(_indexBuffer);
    _indexBuffer = {};
  }
  _vertexBufferAddress = 0;
  return true;
}