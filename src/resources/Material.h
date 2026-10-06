#pragma once

#include <core/vk_types.h>
#include <graphics/utils/vk_descriptors.h>
#include <resources/ResourceManager.h>
#include <resources/Texture.h>
#include <resources/Buffer.h>
#include <string>

class VulkanCore;

// data that is sent to shader for a material instance
struct MaterialConstants
{
	glm::vec4 colorFactors;
	glm::vec4 metalRoughFactors;
	glm::vec4 extraFactors; // x: occlusionStrength, y: normalScale, z, isDoubleSided
	glm::vec4 emissiveFactor; // xyz: emissiveFactor, w: emissiveStrength
	// padding, needed for uniform bnuffers
	glm::vec4 padding[12];
};

class Material : public Resource
{
public:
	enum class TextureSlot : uint32_t
	{
		BaseColor = 0,
		MetallicRoughness = 1,
		Occlusion = 2,
		Normal = 3,
		Emissive = 4
	};

	Material(const std::string& id, ResourceManager* manager, MaterialPassType passType = MaterialPassType::Opaque);
	~Material() override = default;

	void set_texture(const TextureSlot slot, ResourceHandle<Texture> texture);
	void set_constants(const MaterialConstants& constants);
	void update_buffer(VulkanCore* core);

	VkDescriptorSet get_descriptor_set() const { return _descriptorSet; }
	MaterialPassType get_pass_type() const { return _passType; }
	MaterialConstants& get_constants() { return _constants; }
	
	static void init_descriptor_layout(VulkanCore* core);
	static void cleanup_descriptor_layout(VulkanCore* core);
	static VkDescriptorSetLayout get_descriptor_layout() { return _layout; }

protected:
	bool doLoad(VulkanCore* core) override;
	bool doUnload(VulkanCore* core) override;

private:
	ResourceManager* _resourceManager;
	ResourceHandle<Texture> _baseColorTexture;
	ResourceHandle<Texture> _metalRoughTexture;
	ResourceHandle<Texture> _occlusionTexture;
	ResourceHandle<Texture> _normalTexture;
	ResourceHandle<Texture> _emissiveTexture;

	MaterialConstants _constants{};
	MaterialPassType _passType;

	Buffer _materialBuffer{};
	VkDescriptorSet _descriptorSet{ VK_NULL_HANDLE };

	static VkDescriptorSetLayout _layout;
	static DescriptorAllocatorGrowable _descriptorPool;
};