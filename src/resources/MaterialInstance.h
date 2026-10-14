#pragma once

#include "MaterialTemplate.h"
#include <resources/Texture.h>
#include <unordered_map>
#include <string>
#include <vector>
#include <span>
#include <cassert>
#include <cstring>
#include <fmt/core.h>

class MaterialInstance
{
public:
	explicit MaterialInstance(ResourceHandle<MaterialTemplate> materialTemplate)
		: _template(materialTemplate)
	{
		assert(_template.is_valid() && "MaterialInstance: invalid material template recieved");
		_materialBlockSize = _template->get_material_block_size();
		_parameterBuffer.resize(_materialBlockSize, 0);
	}

	template<typename T>
	void set_parameter(const std::string& name, const T& value)
	{
		const auto& reflection = _template->get_reflection();
		auto it = reflection.materialVariablesByName.find(name);

		if (it == reflection.materialVariablesByName.end()) {
			it = reflection.materialVariablesByName.find("materials." + name);		
		}

		if (it != reflection.materialVariablesByName.end()) { // set value in parameter buffer
			assert(sizeof(T) <= it->second.size);
			std::memcpy(_parameterBuffer.data() + it->second.offset, &value, sizeof(T));
			_isDirty = true;
		}
	}

	void set_texture(const std::string& slotName, ResourceHandle<Texture> texture) 
	{
		_textures[slotName] = texture;
		_isDirty = true;

		std::string indexVarName = slotName + "Index";
		const auto& reflection = _template->get_reflection();

		// assign bindless index if variable exists in uniform buffer memory block
		bool hasDirect = reflection.materialVariablesByName.contains(indexVarName);
		bool hasArrayItem = reflection.materialVariablesByName.contains("materials." + indexVarName);

		if (hasDirect || hasArrayItem) {
			uint32_t bindlessIndex = texture.is_valid() ? texture->get_bindless_index() : 0;
			set_parameter<uint32_t>(indexVarName, bindlessIndex);
		}
	}

	void set_alpha_mode(AlphaMode mode) { _alphaMode = mode; }
	void set_double_sided(bool doubleSided) { _doubleSided = doubleSided; }

	AlphaMode get_alpha_mode() const { return _alphaMode; }
	bool is_double_sided() const { return _doubleSided; }

	ResourceHandle<Texture> get_texture(const std::string& slotName) const
	{
		auto it = _textures.find(slotName);
		return (it != _textures.end()) ? it->second : ResourceHandle<Texture>{};
	}

	ResourceHandle<MaterialTemplate> get_template() const { return _template; }
	std::span<const uint8_t> get_parameter_data() const { return _parameterBuffer; }
	bool is_dirty() const { return _isDirty; }
	void clear_dirty() { _isDirty = false; }

private:
	ResourceHandle<MaterialTemplate> _template;
	uint32_t _materialBlockSize{ 0 };
	std::vector<uint8_t> _parameterBuffer;
	std::unordered_map<std::string, ResourceHandle<Texture>> _textures;
	AlphaMode _alphaMode{ AlphaMode::Opaque };
	bool _doubleSided{ false };
	bool _isDirty{ true };
};