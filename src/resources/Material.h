#pragma once

#include <graphics/utils/vk_types.h>
#include <resources/ResourceManager.h>
#include <resources/Texture.h>
#include <string>

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

	Material(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, MaterialPassType passType = MaterialPassType::Opaque);
	~Material() override { Resource::unload(); }

	void set_texture(const TextureSlot slot, ResourceHandle<Texture> texture);
	void set_constants(const MaterialConstants& constants);

	MaterialPassType get_pass_type() const { return _passType; }
	MaterialConstants& get_constants() { return _constants; }
	ResourceHandle<Texture> get_texture(TextureSlot slot) const;
	
protected:
	bool doLoad() override { return true; } // nothing to load, just a container for textures and constats
	bool doUnload() override { return true; } // textures unloaded automatically when their handles go out of scope

private:
	ResourceHandle<Texture> _baseColorTexture;
	ResourceHandle<Texture> _metalRoughTexture;
	ResourceHandle<Texture> _occlusionTexture;
	ResourceHandle<Texture> _normalTexture;
	ResourceHandle<Texture> _emissiveTexture;

	MaterialConstants _constants{};
	MaterialPassType _passType;
};