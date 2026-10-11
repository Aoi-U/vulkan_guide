#include "ResourceManager.h"
#include "GlobalGeometryPool.h"
#include <graphics/VulkanCore.h>

void ResourceManager::init(VulkanCore* core)
{
	_vulkanCore = core;

	init_bindless(); // create bindless descriptor resources

	_geometryPool = std::make_unique<GlobalGeometryPool>();
	_geometryPool->init(_vulkanCore, this);
}

void ResourceManager::cleanup()
{
	if (_geometryPool) {
		_geometryPool->cleanup();
		_geometryPool.reset();
	}

	unload_all();
	if (_vulkanCore) {
		_vulkanCore->get_deletion_queue().flush_all();
	}
	clean_bindless();
}

uint32_t ResourceManager::allocate_bindless_texture(VkImageView view, VkSampler sampler)
{
	std::lock_guard<std::mutex> lock(_bindlessMutex); // ensure thread safety to get a slot

	uint32_t slot = 0;
	if (!_freeBindlessSlots.empty()) { // get a free slot if available
		slot = _freeBindlessSlots.back();
		_freeBindlessSlots.pop_back();
	}
	else { // create new slot
		if (_nextBindlessSlot >= MAX_BINDLESS_TEXTURES) {
			throw std::runtime_error("Exceeded maximum bindless texture slots");
		}
		slot = _nextBindlessSlot++;
	}

	// update descriptor set with new texture at the slot
	VkDescriptorImageInfo imageInfo{};
	imageInfo.imageView = view;
	imageInfo.sampler = sampler;
	imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	VkWriteDescriptorSet write{};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = _bindlessSet;
	write.dstBinding = 0;
	write.dstArrayElement = slot; 
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &imageInfo;

	vkUpdateDescriptorSets(_vulkanCore->get_device(), 1, &write, 0, nullptr);

	return slot;
}

void ResourceManager::free_bindless_slot(uint32_t slot)
{
	if (slot == 0) 
		return;

	std::lock_guard<std::mutex> lock(_bindlessMutex); // ensure thread safety to free slot
	_freeBindlessSlots.push_back(slot); // mark slot as free for reuse
}

void ResourceManager::register_bindless_texture_batched(const std::vector<std::pair<VkImageView, VkSampler>>& textures, std::vector<uint32_t>& outSlots)
{
	if (textures.empty())
		return;

	std::lock_guard<std::mutex> lock(_bindlessMutex);
	outSlots.resize(textures.size());

	std::vector<VkDescriptorImageInfo> imageInfos(textures.size());
	std::vector<VkWriteDescriptorSet> writes(textures.size());

	for (size_t i = 0; i < textures.size(); i++) {
		uint32_t slot = 0;
		if (!_freeBindlessSlots.empty()) { // get a free slot if available
			slot = _freeBindlessSlots.back();
			_freeBindlessSlots.pop_back();
		}
		else { // create new slot
			if (_nextBindlessSlot >= MAX_BINDLESS_TEXTURES) {
				throw std::runtime_error("Exceeded maximum bindless texture slots");
			}
			slot = _nextBindlessSlot++;
		}
		outSlots[i] = slot;

		imageInfos[i] = VkDescriptorImageInfo{
			.sampler = textures[i].second,
			.imageView = textures[i].first,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		};

		writes[i] = VkWriteDescriptorSet{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = _bindlessSet,
			.dstBinding = 0,
			.dstArrayElement = slot,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &imageInfos[i]
		};
	}

	// batch update descriptor set with new textures
	if (!writes.empty()) {
		vkUpdateDescriptorSets(_vulkanCore->get_device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
	}
}

void ResourceManager::queue_main_thread_task(std::function<void()> task)
{
	std::lock_guard<std::mutex> lock(_taskMutex);
	_mainThreadTasks.push_back(std::move(task));
}

void ResourceManager::process_main_thread_tasks()
{
	std::deque<std::function<void()>> tasksToRun;
	{
		std::lock_guard<std::mutex> lock(_taskMutex);
		tasksToRun.swap(_mainThreadTasks); // swap to avoid holding the lock while executing tasks
	}

	while (!tasksToRun.empty()) { // process all queued tasks
		auto task = std::move(tasksToRun.front());
		tasksToRun.pop_front();
		if (task) {
			task();
		}
	}
}

void ResourceManager::unload_unused()
{
	for (auto& [type, typeCounts] : _refCounts) {
		for (auto it = typeCounts.begin(); it != typeCounts.end(); ) {
			if (it->second.refCount <= 0 && !it->second.isPersistent) {
				auto resource = _resources[type][it->first];
				if (_vulkanCore) {
					// add unused resources to deletion queue
					_vulkanCore->get_deletion_queue().push_function([res = resource](){
						res->unload();
						});
				}
				else { 
					resource->unload(); 
				}

				_resources[type].erase(it->first);
				it = typeCounts.erase(it);
			}
			else {
				it++;
			}
		}
	}
}

void ResourceManager::unload_all()
{
	// unload all resources
	for (auto& [type, typeResources] : _resources) {
		for (auto& [id, resource] : typeResources) {
			resource->unload();
		}
	}
	_refCounts.clear();
	_resources.clear();
}

void ResourceManager::init_bindless()
{
	// https://docs.vulkan.org/samples/latest/samples/extensions/descriptor_indexing/README.html
	VkDescriptorSetLayoutBinding binding{};
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = MAX_BINDLESS_TEXTURES; // arbitrary max size
	binding.stageFlags = VK_SHADER_STAGE_ALL;

	// set flags for bindless descriptor set layout
	const VkDescriptorBindingFlags bindFlag =
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
		VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;

	VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
	flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
	flagsInfo.bindingCount = 1;
	flagsInfo.pBindingFlags = &bindFlag;

	// create descriptor set layout with bindless flags
	VkDescriptorSetLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.pNext = &flagsInfo;
	layoutInfo.bindingCount = 1;
	layoutInfo.pBindings = &binding;
	layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;

	VK_CHECK(vkCreateDescriptorSetLayout(_vulkanCore->get_device(), &layoutInfo, nullptr, &_bindlessLayout));

	// https://docs.vulkan.org/refpages/latest/refpages/source/VkDescriptorSetVariableDescriptorCountAllocateInfo.html
	
	VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_BINDLESS_TEXTURES };
	VkDescriptorPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
	poolInfo.maxSets = 1;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes = &poolSize;

	VK_CHECK(vkCreateDescriptorPool(_vulkanCore->get_device(), &poolInfo, nullptr, &_bindlessPool));

	// create descriptor pool for bindless descriptor set
	uint32_t descCount = MAX_BINDLESS_TEXTURES; // arbitrary size
	VkDescriptorSetVariableDescriptorCountAllocateInfo allocFlags{};
	allocFlags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
	allocFlags.descriptorSetCount = 1;
	allocFlags.pDescriptorCounts = &descCount;

	// create descriptor pool for bindless descriptor set
	VkDescriptorSetAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.pNext = &allocFlags;
	allocInfo.descriptorPool = _bindlessPool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &_bindlessLayout;

	VK_CHECK(vkAllocateDescriptorSets(_vulkanCore->get_device(), &allocInfo, &_bindlessSet));
}

void ResourceManager::clean_bindless()
{
	if (_bindlessPool != VK_NULL_HANDLE) {
		vkDestroyDescriptorPool(_vulkanCore->get_device(), _bindlessPool, nullptr);
		_bindlessPool = VK_NULL_HANDLE;
	}
	if (_bindlessLayout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(_vulkanCore->get_device(), _bindlessLayout, nullptr);
		_bindlessLayout = VK_NULL_HANDLE;
	}
}
