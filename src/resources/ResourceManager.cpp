#include "ResourceManager.h"
#include "GlobalGeometryPool.h"
#include "AssetStreamer.h"
#include "DefaultResources.h"
#include <graphics/utils/StagingUploader.h>
#include <graphics/VulkanCore.h>
#include <graphics/vt/VirtualTextureSystem.h>
#include <core/cvars.h>

AutoCVar_Int cvVramBudgetMB("r.vramBudgetMB", "VRAM budget in MB for resource eviction", 2048);

ResourceManager::ResourceManager() = default;
ResourceManager::~ResourceManager() = default;

void ResourceManager::init(VulkanCore* core)
{
	_vulkanCore = core;
	_layoutCache.init(_vulkanCore->get_device());
	init_bindless(); // create bindless descriptor resources

	_stagingUploader = std::make_unique<StagingUploader>();
	_stagingUploader->init(_vulkanCore);

	_assetStreamer = std::make_unique<AssetStreamer>();
	_assetStreamer->init(this, 2);

	_geometryPool = std::make_unique<GlobalGeometryPool>();
	_geometryPool->init(_vulkanCore, this);

	_vtSystem = std::make_unique<VirtualTextureSystem>();
	_vtSystem->init(_vulkanCore, this);
}

void ResourceManager::cleanup()
{
	if (_assetStreamer) {
		_assetStreamer->cleanup();
		_assetStreamer.reset();
	}
	_evictionCandidates.clear();

	if (_vtSystem)
	{
		_vtSystem->cleanup();
		_vtSystem.reset();
	}

	unload_all();

	if (_geometryPool) {
		_geometryPool->cleanup();
		_geometryPool.reset();
	}

	if (_stagingUploader) {
		_stagingUploader->cleanup();
		_stagingUploader.reset();
	}

	unload_all();
	if (_vulkanCore) {
		_vulkanCore->get_deletion_queue().flush_all();
	}

	clean_bindless();
	_layoutCache.cleanup();
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

	// override freed descriptor slot with white image so stale indices never sample garbage
	const auto& defaults = DefaultResources::get();
	if (defaults.whiteImage.is_valid() && _vulkanCore && _bindlessSet != VK_NULL_HANDLE) {
		VkDescriptorImageInfo fallbackInfo{
			.sampler = defaults.whiteImage->get_sampler(),
			.imageView = defaults.whiteImage->get_image_view(),
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		};
		VkWriteDescriptorSet write{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = _bindlessSet,
			.dstBinding = 0,
			.dstArrayElement = slot,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &fallbackInfo
		};
		vkUpdateDescriptorSets(_vulkanCore->get_device(), 1, &write, 0, nullptr);
	}

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

void ResourceManager::process_evictions(uint64_t currentFrame)
{
	_currentFrame = currentFrame;
	if (_evictionCandidates.empty())
		return;

	// compute current tracked vram usage against r.vramBudgetMB
	uint64_t totalGpuBytes = 0;
	for (const auto& meta : AssetRegistry::get().get_all_assets()) {
		totalGpuBytes += meta.gpuSizeBytes;
	}
	const uint64_t budgetBytes = static_cast<uint64_t>(std::max(128, cvVramBudgetMB.Get())) * 1024ULL * 1024ULL;
	const bool overBudget = (totalGpuBytes > budgetBytes);

	for (auto it = _evictionCandidates.begin(); it != _evictionCandidates.end();) {
		auto typeIt = _refCounts.find(it->typeKey);
		if (typeIt == _refCounts.end()) {
			it = _evictionCandidates.erase(it);
			continue;
		}

		auto resIt = typeIt->second.find(it->uuid);
		if (resIt == typeIt->second.end() || resIt->second.refCount > 0) {
			it = _evictionCandidates.erase(it);
			continue;
		}

		const bool graceExpired = (currentFrame >= it->zeroRefFrame + EVICTION_FRAME_THRESHOLD);

		// unload resource if grace period expired or over vram budget
		if (graceExpired || overBudget) {
			queue_unload(_resources[it->typeKey][it->uuid]);
			_resources[it->typeKey].erase(it->uuid); // remove from resource map
			typeIt->second.erase(resIt); // remove from ref count map
			it = _evictionCandidates.erase(it);
		}
		else {
			it++;
		}
	}
}

void ResourceManager::process_evictions_immediate()
{
	for (auto it = _evictionCandidates.begin(); it != _evictionCandidates.end();) {
		auto typeIt = _refCounts.find(it->typeKey);
		if (typeIt == _refCounts.end()) {
			it = _evictionCandidates.erase(it);
			continue;
		}

		auto resIt = typeIt->second.find(it->uuid);
		if (resIt == typeIt->second.end() || resIt->second.refCount > 0) {
			it = _evictionCandidates.erase(it);
			continue;
		}
		
		// unload zero ref resource immediately
		queue_unload(_resources[it->typeKey][it->uuid]);
		_resources[it->typeKey].erase(it->uuid);
		typeIt->second.erase(resIt);
		it = _evictionCandidates.erase(it);
	}
}

void ResourceManager::request_async_load(const UUID& uuid)
{
	if (_assetStreamer) {
		_assetStreamer->request_load(uuid);
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

void ResourceManager::queue_unload(std::shared_ptr<Resource> resource)
{
	if (_vulkanCore) {
		_vulkanCore->get_deletion_queue().push_function([res = std::move(resource)]() {
			res->unload();
			});
	}
	else {
		resource->unload();
	}
}
