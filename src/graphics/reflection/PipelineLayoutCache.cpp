#include "PipelineLayoutCache.h"

#include <graphics/utils/vk_types.h>
#include <fmt/core.h>

size_t DescriptorSetLayoutDescHash::operator()(const DescriptorSetLayoutDesc& desc) const
{
	size_t hash = std::hash<size_t>()(desc.bindings.size());
	for (size_t i = 0; i < desc.bindings.size(); i++) { // hash-combine binding properties
		const VkDescriptorSetLayoutBinding& binding = desc.bindings[i];
		hash = hash_combine(hash, std::hash<uint32_t>()(binding.binding));
		hash = hash_combine(hash, std::hash<uint32_t>()(binding.descriptorType));
		hash = hash_combine(hash, std::hash<uint32_t>()(binding.descriptorCount));
		hash = hash_combine(hash, std::hash<uint32_t>()(binding.stageFlags));
	}
	return hash;
}

size_t PipelineLayoutKeyHash::operator()(const PipelineLayoutKey& key) const
{
	size_t hash = std::hash<size_t>()(key.setLayouts.size());
	for (auto s : key.setLayouts) {
		hash ^= std::hash<uint64_t>()(reinterpret_cast<uint64_t>(s)) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
	}
	for (const VkPushConstantRange& range : key.pushConstantRanges) {
		hash = hash_combine(hash, std::hash<uint32_t>()(range.offset ^ range.size ^ range.stageFlags));
	}
	return hash;
}

void PipelineLayoutCache::init(VkDevice device)
{
	_device = device;
}

void PipelineLayoutCache::cleanup()
{
	if (_device == VK_NULL_HANDLE) {
		fmt::println("PipelineLayoutCache::cleanup called with null device");
		return;
	}

	for (auto& [_, layout] : _setLayoutCache) { // destroy all cached descriptor set layouts
		if (layout != VK_NULL_HANDLE) {
			vkDestroyDescriptorSetLayout(_device, layout, nullptr);
		}
	}

	for (auto& [_, layout] : _pipelineLayoutCache) { // destroy all cached pipeline layouts
		if (layout != VK_NULL_HANDLE) {
			vkDestroyPipelineLayout(_device, layout, nullptr);
		}
	}

	_setLayoutCache.clear();
	_pipelineLayoutCache.clear();
	_device = VK_NULL_HANDLE;
}

VkDescriptorSetLayout PipelineLayoutCache::get_descriptor_set_layout(const std::vector<ReflectedBinding>& reflectedBindings, bool isBindless)
{
	// create a DescriptorSetLayoutDesc from the reflected bindings
	DescriptorSetLayoutDesc desc;
	bool hasBindingFlags = false;
	bool hasUpdateAfterBind = false;

	for (const auto& binding : reflectedBindings) {
		VkDescriptorSetLayoutBinding layoutBinding{
			.binding = binding.binding,
			.descriptorType = binding.descriptorType,
			.descriptorCount = binding.descriptorCount,
			.stageFlags = binding.stageFlags,
			.pImmutableSamplers = nullptr
		};
		desc.bindings.push_back(layoutBinding);

		VkDescriptorBindingFlags flags = 0;
		if (isBindless || binding.isVariableCount) {
			flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
			hasBindingFlags = true;
			hasUpdateAfterBind = true;
		}
		desc.bindingFlags.push_back(flags);
	}

	auto it = _setLayoutCache.find(desc);
	if (it != _setLayoutCache.end()) { // get existing layout from cache
		return it->second;
	}

	// create new descriptor set layout
	VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
		.pNext = nullptr,
		.bindingCount = static_cast<uint32_t>(desc.bindingFlags.size()),
		.pBindingFlags = desc.bindingFlags.data()
	};

	VkDescriptorSetLayoutCreateInfo createInfo{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.pNext = hasBindingFlags ? &flagsInfo : nullptr,
		.flags = (hasUpdateAfterBind || isBindless) ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0u,
		.bindingCount = static_cast<uint32_t>(desc.bindings.size()),
		.pBindings = desc.bindings.data()
	};

	// create and cache new layout
	VkDescriptorSetLayout layout;
	VK_CHECK(vkCreateDescriptorSetLayout(_device, &createInfo, nullptr, &layout));
	_setLayoutCache[desc] = layout;

	return layout;
}

VkPipelineLayout PipelineLayoutCache::get_pipeline_layout(const std::vector<VkDescriptorSetLayout>& setLayouts, const std::vector<VkPushConstantRange>& pushRanges)
{
	PipelineLayoutKey key{
		.setLayouts = setLayouts,
		.pushConstantRanges = pushRanges
	};

	auto it = _pipelineLayoutCache.find(key);
	if (it != _pipelineLayoutCache.end()) { // get existing layout from cache
		return it->second;
	}

	// create new pipeline layout
	VkPipelineLayoutCreateInfo layoutInfo{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.setLayoutCount = static_cast<uint32_t>(setLayouts.size()),
		.pSetLayouts = setLayouts.data(),
		.pushConstantRangeCount = static_cast<uint32_t>(pushRanges.size()),
		.pPushConstantRanges = pushRanges.data()
	};
	
	// create and cache new layout
	VkPipelineLayout layout;
	VK_CHECK(vkCreatePipelineLayout(_device, &layoutInfo, nullptr, &layout));
	_pipelineLayoutCache[key] = layout;

	return layout;
}
