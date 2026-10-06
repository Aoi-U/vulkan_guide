#pragma once

#include <core/vk_types.h>
#include <filesystem>
#include <memory>
#include <resources/ResourceManager.h>
#include <string>
#include <string_view>

class Shader : public Resource
{
public:
	Shader(const std::string& id, VkShaderStageFlagBits stage, std::string_view entryPoint, const std::filesystem::path& customPath);
	~Shader() override = default;

	// getters
	VkShaderModule get_module() const { return _module; }
	VkShaderStageFlagBits get_stage() const { return _stage; }
	VkPipelineShaderStageCreateInfo get_stage_info() const;

protected:
	bool doLoad(VulkanCore* core) override;
	bool doUnload(VulkanCore* core) override;

private:
	std::filesystem::path _filePath{};
	VkShaderModule _module{ VK_NULL_HANDLE };
	VkShaderStageFlagBits _stage{};
	std::string _entryPoint{ "main" };
};