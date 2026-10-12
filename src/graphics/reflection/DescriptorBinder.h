#pragma once

#include "ShaderReflectionTypes.h"
#include <vulkan/vulkan.h>
#include <unordered_map>
#include <string>
#include <vector>

class DescriptorBinder
{
public:
	// updates given descriptor set with buffer and image infos based on reflected bindings
	static void update_set(VkDevice device, VkDescriptorSet targetSet, const std::vector<ReflectedBinding>& setBindings,
		const std::unordered_map<std::string, VkDescriptorBufferInfo>& bufferInfos, const std::unordered_map<std::string, VkDescriptorImageInfo>& imageInfos)
	{
		std::vector<VkWriteDescriptorSet> writes;
		writes.reserve(setBindings.size());

		for (const ReflectedBinding& binding : setBindings) {
			VkWriteDescriptorSet write{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = targetSet,
				.dstBinding = binding.binding,
				.dstArrayElement = 0,
				.descriptorCount = binding.descriptorCount,
				.descriptorType = binding.descriptorType
			};

			if (binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
				auto it = bufferInfos.find(binding.name);
				if (it != bufferInfos.end()) {
					write.pBufferInfo = &it->second;
					writes.push_back(write);
				}
			}
			else if (binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
				binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
				binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
				auto it = imageInfos.find(binding.name);
				if (it != imageInfos.end()) {
					write.pImageInfo = &it->second;
					writes.push_back(write);
				}
			}
		}

		if (!writes.empty()) {
			vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
		}
	}
};