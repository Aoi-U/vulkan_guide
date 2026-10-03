#include "graphics/Shader.h"
#include <utils/vk_initializers.h>
#include <fstream>
#include <fmt/core.h>

Shader::Shader(VkDevice device, VkShaderStageFlagBits stage, std::string_view filePath, std::string_view entryPoint)
	: _device(device), _stage(stage), _entryPoint(entryPoint)
{
	// open the file, with cursor at end
	std::ifstream file(filePath.data(), std::ios::ate | std::ios::binary);

	if (!file.is_open()) {
		fmt::print("Shader: Failed to open shader file: {}\n", filePath);
		return;
	}

	// find what the size of the file is by looking up the location of the cursor
	// because the cursor is at the end, it gives the size directly in bytes
	size_t fileSize = (size_t)file.tellg();

	// spirv expects the buffer to be on uint32, so make sure to reserve an int vector big enough for the entire file
	std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));

	// put file cursor at beginning
	file.seekg(0);

	// load the entire file into the buffer
	file.read((char*)buffer.data(), fileSize);

	// close the file now that it is loaded into the buffer
	file.close();

	// create a new shader module, using the buffer we loaded
	VkShaderModuleCreateInfo createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	createInfo.pNext = nullptr;

	// codeSize has to be in bytes, so multiply the ints in the buffer by size of int
	// to know the real size of buffer
	createInfo.codeSize = buffer.size() * sizeof(uint32_t);
	createInfo.pCode = buffer.data();

	// check that the creation goes well
	if (vkCreateShaderModule(_device, &createInfo, nullptr, &_module) != VK_SUCCESS) {
		fmt::println("Shader: Failed to create shader module from file: {}", filePath);
		_module = VK_NULL_HANDLE;
	}
}

Shader::~Shader()
{
	if (_module != VK_NULL_HANDLE && _device != VK_NULL_HANDLE) {
		vkDestroyShaderModule(_device, _module, nullptr);
		_module = VK_NULL_HANDLE;
	}
}

std::shared_ptr<Shader> Shader::load(VkDevice device, VkShaderStageFlagBits stage, std::string_view filePath, std::string_view entryPoint)
{
	auto shader = std::make_shared<Shader>(device, stage, filePath, entryPoint);
	return shader->is_valid() ? shader : nullptr;
}

VkPipelineShaderStageCreateInfo Shader::get_stage_info() const
{
	return vkinit::pipeline_shader_stage_create_info(_stage, _module, _entryPoint.c_str());
}
