#pragma once

#include <core/vk_types.h>
#include <string>
#include <string_view>
#include <memory>

class Shader
{
public:
	Shader(VkDevice device, VkShaderStageFlagBits stage, std::string_view filePath, std::string_view entryPoint = "main");
	~Shader();

	// loads shader from file and create a shader module
	static std::shared_ptr<Shader> load(VkDevice device, VkShaderStageFlagBits stage, std::string_view filePath, std::string_view entryPoint = "main");

	// getters
	VkShaderModule get_module() const { return _module; }
	VkShaderStageFlagBits get_stage() const { return _stage; }
	const std::string& get_entry_point() const { return _entryPoint; }
	VkPipelineShaderStageCreateInfo get_stage_info() const;
	bool is_valid() const { return _module != VK_NULL_HANDLE; }

private:
	VkDevice _device{ VK_NULL_HANDLE };
	VkShaderModule _module{ VK_NULL_HANDLE };
	VkShaderStageFlagBits _stage{};
	std::string _entryPoint{ "main" };
};