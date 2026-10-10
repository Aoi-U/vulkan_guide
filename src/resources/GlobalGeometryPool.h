#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <resources/ResourceManager.h>
#include <resources/Buffer.h>
#include <graphics/utils/vk_types.h>

class VulkanCore;

// allocation of geometry data in the global geometry pool
struct GeometryAllocation
{
	uint32_t vertexOffset;
	uint32_t firstIndex;
	uint32_t vertexCount;
	uint32_t indexCount;
	VkDeviceAddress vertexBufferAddress;
};

class GlobalGeometryPool
{
public:
	void init(VulkanCore* core, ResourceManager* rm);
	void cleanup();

	GeometryAllocation allocate(uint32_t vertexCount, uint32_t indexCount);
	void free(const GeometryAllocation& alloc);

	VkBuffer get_vertex_buffer() const;
	VkBuffer get_index_buffer() const;
	VkDeviceAddress get_vertex_buffer_address() const { return _vertexBufferAddress; }

private:
	VulkanCore* _core{ nullptr };
	ResourceManager* _rm{ nullptr };
	ResourceHandle<Buffer> _globalVertexBuffer;
	ResourceHandle<Buffer> _globalIndexBuffer;
	VkDeviceAddress _vertexBufferAddress{ 0 };

	uint32_t _nextVertexOffset{ 0 };
	uint32_t _nextIndexOffset{ 0 };
};