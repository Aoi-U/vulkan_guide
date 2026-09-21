#pragma once

#include <core/vk_types.h>
#include <graphics/Materials.h>

#include <entt.hpp>

struct RenderStats
{
  int drawcallCount{ 0 };
  int triangleCount{ 0 };
  float meshDrawTime{ 0.0f };
};

struct FrameData;
class VulkanCore;

class MeshRenderer
{
public:
  void init(VulkanCore* vulkanCore, VkFormat colorFormat, VkFormat depthFormat);
  void cleanup();

  void render(VkCommandBuffer cmd, FrameData& frameData, const entt::registry& registry, const GPUSceneData& sceneData, VkExtent2D drawExtent);

  GLTFMetallic_Roughness* get_material_system() { return &_metalRoughMaterial; }
	const RenderStats& get_stats() const { return _stats; }

private:
  // determine if a render object is visible in the current view frustum
  bool is_visible(const RenderObject& obj, const glm::mat4& viewProj);

	void collect_draw_nodes(const std::shared_ptr<Node>& node, const glm::mat4& parentMatrix, std::vector<RenderObject>& opaqueObjects, std::vector<RenderObject>& transparentObjects);

  VulkanCore* _vulkanCore;
  VkDescriptorSetLayout _sceneDataDescriptorLayout;
  GLTFMetallic_Roughness _metalRoughMaterial;
	RenderStats _stats;
};