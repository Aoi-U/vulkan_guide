#pragma once

#include <vulkan/vulkan.h>
#include "ShaderReflectionTypes.h"
#include <vector>
#include <string>
#include <cstring>
#include <cassert>

class PushConstantDispatcher
{
public:
	explicit PushConstantDispatcher(const PipelineLayoutReflection& reflection) 
		: _reflection(reflection)
	{
		uint32_t totalSize = 0;
		for (const auto& range : _reflection.pushConstantRanges) 
		{
			totalSize = std::max(totalSize, range.offset + range.size);
		}

		_buffer.resize(totalSize, 0);
	}

	template<typename T>
	void set(const std::string& name, const T& value) // sets value of push constant by name
	{
		auto it = _reflection.pushConstantsByName.find(name);
		if (it != _reflection.pushConstantsByName.end()) {
			assert(sizeof(T) <= it->second.size);
			std::memcpy(_buffer.data() + it->second.offset, &value, sizeof(T));
		}
	}

	void flush(VkCommandBuffer cmd, VkPipelineLayout layout) const // flush push constants to command buffer
	{
		for (const auto& range : _reflection.pushConstantRanges) {
			vkCmdPushConstants(cmd, layout, range.stageFlags, range.offset, range.size, _buffer.data() + range.offset);
		}
	}


private:
	const PipelineLayoutReflection& _reflection;
	std::vector<uint8_t> _buffer;
};