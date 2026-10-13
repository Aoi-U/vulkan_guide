#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <unordered_map>
#include "ShaderReflectionTypes.h"

struct DescriptorSetLayoutDesc
{
	std::vector<VkDescriptorSetLayoutBinding> bindings;
	std::vector<VkDescriptorBindingFlags> bindingFlags;

	bool operator==(const DescriptorSetLayoutDesc& other) const
	{
		if (bindings.size() != other.bindings.size() || bindingFlags.size() != other.bindingFlags.size())
			return false;

		for (size_t i = 0; i < bindings.size(); i++) {
			if (bindings[i].binding != other.bindings[i].binding ||
					bindings[i].descriptorType != other.bindings[i].descriptorType ||
					bindings[i].descriptorCount != other.bindings[i].descriptorCount ||
					bindings[i].stageFlags != other.bindings[i].stageFlags) {
				return false;
			}

			if (bindingFlags[i] != other.bindingFlags[i]) {
				return false;
			}

			return true;
		}
	}
};

struct DescriptorSetLayoutDescHash // hash function for DescriptorSetLayoutDesc
{
	size_t operator()(const DescriptorSetLayoutDesc& desc) const;
};

struct PipelineLayoutKey
{
	std::vector<VkDescriptorSetLayout> setLayouts;
	std::vector<VkPushConstantRange> pushConstantRanges;

	bool operator==(const PipelineLayoutKey& other) const 
	{
		if (setLayouts != other.setLayouts)
			return false;

		if (pushConstantRanges.size() != other.pushConstantRanges.size())
			return false;

		for (size_t i = 0; i < pushConstantRanges.size(); i++) {
			if (pushConstantRanges[i].stageFlags != other.pushConstantRanges[i].stageFlags ||
					pushConstantRanges[i].offset != other.pushConstantRanges[i].offset ||
					pushConstantRanges[i].size != other.pushConstantRanges[i].size) {
				return false;
			}
		}

		return true;
	}
};

struct PipelineLayoutKeyHash // hash function for PipelineLayoutKey
{
	size_t operator()(const PipelineLayoutKey& key) const;
};

class PipelineLayoutCache
{
public:
	void init(VkDevice device);
	void cleanup();

	VkDescriptorSetLayout get_descriptor_set_layout(const std::vector<ReflectedBinding>& reflectedBindings, bool isBindless = false);
	VkPipelineLayout get_pipeline_layout(const std::vector<VkDescriptorSetLayout>& setLayouts, const std::vector<VkPushConstantRange>& pushRanges);

private:
	VkDevice _device{ VK_NULL_HANDLE };
	std::unordered_map<DescriptorSetLayoutDesc, VkDescriptorSetLayout, DescriptorSetLayoutDescHash> _setLayoutCache;
	std::unordered_map<PipelineLayoutKey, VkPipelineLayout, PipelineLayoutKeyHash> _pipelineLayoutCache;
};