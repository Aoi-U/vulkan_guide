#pragma once

#include <graphics/utils/vk_types.h>
#include <filesystem>
#include <memory>
#include <resources/ResourceManager.h>
#include <string>
#include <string_view>

class Shader : public Resource
{
public:
	Shader(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, VkShaderStageFlagBits stage, std::string_view entryPoint, const std::filesystem::path& filePath  = "");
	~Shader() override { Resource::unload(); }

	// getters
	VkShaderModule get_module() const { return _module; }
	VkShaderStageFlagBits get_stage() const { return _stage; }
	VkPipelineShaderStageCreateInfo get_stage_info() const;
	std::span<const uint32_t> get_spirv_code() const { return _spirvCode; }

protected:
	bool doLoad() override;
	bool doUnload() override;

private:
	std::filesystem::path _filePath{""};
	VkShaderModule _module{ VK_NULL_HANDLE };
	VkShaderStageFlagBits _stage{};
	std::string _entryPoint{ "main" };
	std::vector<uint32_t> _spirvCode; // spirv bytecode for reflection
};