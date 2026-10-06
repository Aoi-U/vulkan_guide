#include "resources/Shader.h"
#include <utils/vk_initializers.h>
#include <graphics/VulkanCore.h>
#include <fstream>
#include <fmt/core.h>

Shader::Shader(const std::string& id, VkShaderStageFlagBits stage, std::string_view entryPoint, const std::filesystem::path& customPath)
	: Resource(id), _stage(stage), _entryPoint(entryPoint), _filePath(customPath)
{
	// resolve fallback spirv file path
	if (_filePath.empty()) {
		std::string extension;
		switch (_stage) {
		case VK_SHADER_STAGE_VERTEX_BIT: extension = ".vert"; break;
		case VK_SHADER_STAGE_FRAGMENT_BIT: extension = ".frag"; break;
		case VK_SHADER_STAGE_COMPUTE_BIT: extension = ".comp"; break;
		default: return; // unsupported fallback path
		}
		_filePath = std::filesystem::path("../../shaders") / (id + extension + ".spv");
	}
}

VkPipelineShaderStageCreateInfo Shader::get_stage_info() const
{
	VkPipelineShaderStageCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.pNext = nullptr;
	info.stage = _stage;
	info.module = _module;
	info.pName = _entryPoint.c_str();
	return info;
}

bool Shader::doLoad(VulkanCore* core)
{
	// open the file, with cursor at end
	std::ifstream file(_filePath, std::ios::ate | std::ios::binary);

	if (!file.is_open()) {
		fmt::print("Shader: Failed to open shader file: {}\n", _filePath);
		return false;
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

	if (vkCreateShaderModule(core->get_device(), &createInfo, nullptr, &_module) != VK_SUCCESS) {
		fmt::println("Shader: Failed to create shader module from file: {}", _filePath);
		return false;
	}

	return true;
}

bool Shader::doUnload(VulkanCore* core)
{
	if (_module != VK_NULL_HANDLE) {
		vkDestroyShaderModule(core->get_device(), _module, nullptr);
		_module = VK_NULL_HANDLE;
	}
	return true;
}
