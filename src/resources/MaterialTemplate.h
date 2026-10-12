#pragma once

#include <resources/ResourceManager.h>
#include <resources/Shader.h>
#include <graphics/reflection/PipelineLayoutCache.h>
#include <graphics/Pipeline.h>
#include <graphics/utils/vk_types.h>

struct MaterialPipelineKey
{
	VkCullModeFlags cullMode{ VK_CULL_MODE_BACK_BIT };
	VkFrontFace frontFace{ VK_FRONT_FACE_CLOCKWISE };
	VkPolygonMode polygonMode{ VK_POLYGON_MODE_FILL };
	BlendMode blendMode{ BlendMode::None };
	bool depthTest{ true };
	bool depthWrite{ true };
	VkCompareOp depthComareOp{ VK_COMPARE_OP_GREATER_OR_EQUAL };
	std::vector<VkFormat> colorAttachmentFormats;
	VkFormat depthAttachmentFormat{ VK_FORMAT_UNDEFINED };
	
	bool operator==(const MaterialPipelineKey& key) const = default;
};

struct MaterialPipelineKeyHash // hash function for MaterialPipelineKey
{
	size_t operator()(const MaterialPipelineKey& key) const
	{
		size_t hash = std::hash<uint32_t>()(key.cullMode ^ key.polygonMode);
		hash ^= std::hash<uint32_t>()(static_cast<uint32_t>(key.blendMode)) << 2;
		hash ^= std::hash<uint32_t>()(key.depthAttachmentFormat) << 4;
		for (auto format : key.colorAttachmentFormats)
		{
			hash = hash_combine(hash, std::hash<uint32_t>()(format));
		}
		return hash;
	}
};

class MaterialTemplate : public Resource
{
public:
	MaterialTemplate(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, ResourceHandle<Shader> vertShader, ResourceHandle<Shader> fragShader);

	~MaterialTemplate() override { Resource::unload(); }

	VkPipeline get_pipeline(const MaterialPipelineKey& key);
	VkPipelineLayout get_layout() const { return _pipelineLayout; }
	const PipelineLayoutReflection& get_reflection() const { return _reflection; }
	uint32_t get_material_block_size() const { return _materialBlockSize; }

protected:
	bool doLoad() override;
	bool doUnload() override;

private:
	ResourceHandle<Shader> _vertShader;
	ResourceHandle<Shader> _fragShader;
	PipelineLayoutReflection _reflection;
	VkPipelineLayout _pipelineLayout{ VK_NULL_HANDLE };
	std::vector<VkDescriptorSetLayout> _descriptorSetLayouts;
	uint32_t _materialBlockSize;

	std::unordered_map<MaterialPipelineKey, VkPipeline, MaterialPipelineKeyHash> _pipelineCache;
};