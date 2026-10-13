#include "resources/Shader.h"
#include <utils/vk_initializers.h>
#include <graphics/VulkanCore.h>
#include <fstream>
#include <fmt/core.h>

Shader::Shader(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, VkShaderStageFlagBits stage, std::string_view entryPoint, const std::filesystem::path& filePath)
	: Resource(id, core, resourceManager), _stage(stage), _entryPoint(entryPoint)
{
	// resolve fallback spirv file path
	if (_filePath.empty() && !id.empty()) {
		const std::filesystem::path baseDir = "../../assets/shaders";
		std::filesystem::path p(id);

		// If the ID already has an extension like .spv, use it directly
		if (p.extension() == ".spv") {
			_filePath = baseDir / p;
		}
		else {
			// Append .spv (e.g., "gbuffer.vert" -> "gbuffer.vert.spv")
			_filePath = baseDir / (id + ".spv");
		}
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

bool Shader::doLoad()
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

	if (vkCreateShaderModule(_core->get_device(), &createInfo, nullptr, &_module) != VK_SUCCESS) {
		fmt::println("Shader: Failed to create shader module from file: {}", _filePath);
		return false;
	}

	_spirvCode = std::move(buffer);

	return true;
}

bool Shader::doUnload()
{
	if (_module != VK_NULL_HANDLE) {
		_core->get_deletion_queue().push(_module);
		_module = VK_NULL_HANDLE;
	}
	_spirvCode.clear();

	return true;
}
