#pragma once

#include "ShaderReflectionTypes.h"
#include <span>
#include <vector>

class ShaderReflection
{
public:
	static bool reflect_stage(std::span<const uint32_t> spirv, VkShaderStageFlagBits stage, ReflectedShaderStage& outStage);

	static PipelineLayoutReflection merge_stages(std::span<const ReflectedShaderStage> stages);
};