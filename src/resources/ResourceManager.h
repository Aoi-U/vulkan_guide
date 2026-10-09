#pragma once

#include <vulkan/vulkan.h>
#include <string>
#include <optional>
#include <unordered_map>
#include <memory>
#include <typeindex>
#include <vector>
#include <type_traits>
#include <utility>
#include <typeinfo>
#include <fmt/std.h>
#include <assert.h>
#include <mutex>

// resource management classes heavily based off vulkan resource management tutorial: 
// https://docs.vulkan.org/tutorial/latest/Building_a_Simple_Engine/Engine_Architecture/04_resource_management.html#_resource_management

class VulkanCore;
class ResourceManager;

// handle to a resource, managed by resource manager
template<typename T>
class ResourceHandle
{
public:
	ResourceHandle();
	ResourceHandle(const std::string& id, ResourceManager* manager);
	//ResourceHandle(const ResourceHandle& other);
	//ResourceHandle& operator=(const ResourceHandle& other);
	//ResourceHandle(ResourceHandle&& other) noexcept;
	//ResourceHandle& operator=(ResourceHandle&& other) noexcept;
	//~ResourceHandle();

	T* get() const;
	bool is_valid() const;

	const std::string& get_id() const { return resourceId; }

	// Declarations only
	T* operator->() const;
	T& operator*() const;
	operator bool() const;

private:
	std::string resourceId;
	ResourceManager* resourceManager;
};

// resource base class
class Resource
{
public:
	explicit Resource(const std::string& id, VulkanCore* core, ResourceManager* manager)
		: resourceId(id), _core(core), _resourceManager(manager) {}

	virtual ~Resource() = default;

	const std::string& get_id() const { return resourceId; }
	bool is_loaded() const { return loaded; }

	bool load()
	{
		if (loaded)
			return true;

		loaded = doLoad();
		return loaded;
	}

	void unload()
	{
		if (!loaded)
			return;

		doUnload();
		loaded = false;
		
	}

protected:
	virtual bool doLoad() = 0;
	virtual bool doUnload() = 0;

	Resource() = default;
	Resource(Resource&& other) noexcept
		: resourceId(std::move(other.resourceId)), loaded(other.loaded), _core(other._core)
	{
		other.loaded = false;
		other._core = nullptr;
		other._resourceManager = nullptr;
	}

	Resource& operator=(Resource&& other) noexcept {
		if (this != &other) {
			resourceId = std::move(other.resourceId);
			loaded = other.loaded;
			_core = other._core;
			_resourceManager = other._resourceManager;

			other.loaded = false;
			other._core = nullptr;
			other._resourceManager = nullptr;
		}
		return *this;
	}
	Resource(const Resource&) = delete;
	Resource& operator=(const Resource&) = delete;

	VulkanCore* _core{ nullptr };
	ResourceManager* _resourceManager{ nullptr };

private:
	std::string resourceId;
	bool loaded = false;

};

// resource manager
class ResourceManager
{
public:
	// two level ref count for automatic resource lifecycle management
	// maps resource type first, then resource id to their data
	struct ResourceData
	{
		std::shared_ptr<Resource> resource; // the actual resource
		int refCount; // reference count for this resource
		bool isPersistent{ false }; // marks asset for persistent preloading
	};
	
	void init(VulkanCore* core);

	void cleanup();

	uint32_t allocate_bindless_texture(VkImageView view, VkSampler sampler);

	void free_bindless_slot(uint32_t slot);

	VkDescriptorSetLayout get_bindless_layout() const { return _bindlessLayout; }
	VkDescriptorSet get_bindless_set() const { return _bindlessSet; }

	template<typename T, typename... Args>
	ResourceHandle<T> preload(const std::string& resourceId, Args&&... args) {
		auto handle = load<T>(resourceId, std::forward<Args>(args)...);
		set_persistent<T>(resourceId, true);
		return handle;
	}

	template<typename T, typename... Args>
	ResourceHandle<T> load(const std::string& resourceId, Args&&... args)
	{
		static_assert(std::is_base_of<Resource, T>::value, "T must derive from Resource");
		auto typeKey = std::type_index(typeid(T));

		// check existing resource cache to avoid redundant loading
		auto& typeResources = _resources[typeKey];
		auto it = typeResources.find(resourceId);

		if (it != typeResources.end()) {
			// resource exists in cache, increment reference count and return handle
			_refCounts[typeKey][resourceId].refCount++;
			return ResourceHandle<T>(resourceId, this);
		}

		// create new resource instance and attempt loading
		auto resource = std::make_shared<T>(resourceId, _vulkanCore, this, std::forward<Args>(args)...);
		if (!resource->load()) {
			return ResourceHandle<T>(); // return invalid handle if loading fails
		}

		// cache successful resource and initialize reference tracking
		typeResources[resourceId] = resource;
		_refCounts[typeKey][resourceId] = ResourceData{ resource, 1 };

		return ResourceHandle<T>(resourceId, this);
	}

	template<typename T>
	void set_persistent(const std::string& resourceId, bool persistent) {
		auto typeKey = std::type_index(typeid(T));
		if (_refCounts.contains(typeKey) && _refCounts[typeKey].contains(resourceId)) {
			_refCounts[typeKey][resourceId].isPersistent = persistent;
		}
	}

	template<typename T>
	bool is_persistent(const std::string& resourceId) const {
		auto typeKey = std::type_index(typeid(T));
		if (_refCounts.contains(typeKey) && _refCounts.at(typeKey).contains(resourceId)) {
			return _refCounts.at(typeKey).at(resourceId).isPersistent;
		}
		return false;
	}

	template<typename T>
	T* get_resource(const std::string& resourceId)
	{
		// access type specific resource container 
		auto& typeResources = _resources[std::type_index(typeid(T))];
		auto it = typeResources.find(resourceId);

		if (it != typeResources.end()) {
			// resource found, perform safe downcast and return typed pointer
			return static_cast<T*>(it->second.get());
		}

		// resource not found, return null
		return nullptr;
	}

	template<typename T>
	std::vector<std::string> get_loaded_resources() const
	{
		std::vector<std::string> keys;
		auto typeKey = std::type_index(typeid(T));
		auto it = _resources.find(typeKey);
		if (it != _resources.end()) {
			for (const auto& [id, _] : it->second) {
				keys.push_back(id);
			}
		}
		return keys;
	}

	template<typename T>
	bool has_resource(const std::string& resourceId)
	{
		auto resourceTypeIt = _resources.find(std::type_index(typeid(T)));
		if (resourceTypeIt != _resources.end()) {
			return resourceTypeIt->second.contains(resourceId);
		}
		return false;
	}

	template<typename T>
	void add_ref(const std::string& resourceId) 
	{
		auto typeKey = std::type_index(typeid(T));
		auto typeIt = _refCounts.find(typeKey);
		if (typeIt != _refCounts.end()) {
			auto it = typeIt->second.find(resourceId);
			if (it != typeIt->second.end()) {
				it->second.refCount++;
			}
		}
	}

	template<typename T>
	void release(const std::string& resourceId)
	{
		auto typeKey = std::type_index(typeid(T));

		// find the map for type T
		auto typeIt = _refCounts.find(typeKey);
		if (typeIt == _refCounts.end())
			return;

		// locate reference count for this resource
		auto it = typeIt->second.find(resourceId);

		if (it == typeIt->second.end())
			return;

		it->second.refCount--;

		// check if resource has no remaining references
		if (it->second.refCount <= 0 && !it->second.isPersistent) {
			auto resTypeIt = _resources.find(typeKey);

			if (resTypeIt == _resources.end())
				return;

			auto& typeResources = resTypeIt->second;
			auto resourceIt = typeResources.find(resourceId);

			if (resourceIt == typeResources.end())
				return;

			resourceIt->second->unload();
			typeResources.erase(resourceIt);
			typeIt->second.erase(it);
		}
	}

	template<typename T>
	void force_unload(const std::string& resourceId) {
		auto typeKey = std::type_index(typeid(T));
		if (_resources.contains(typeKey) && _resources[typeKey].contains(resourceId)) {
			_resources[typeKey][resourceId]->unload();
			_resources[typeKey].erase(resourceId);
			_refCounts[typeKey].erase(resourceId);
		}
	}

	void unload_unused() {
		for (auto& [type, typeCounts] : _refCounts) {
			for (auto it = typeCounts.begin(); it != typeCounts.end(); ) {
				if (it->second.refCount <= 0 && !it->second.isPersistent) {
					_resources[type][it->first]->unload();
					_resources[type].erase(it->first);
					it = typeCounts.erase(it);
				}
				else {
					++it;
				}
			}
		}
	}

	void unload_all()
	{
		for (auto& [type, typeResources] : _resources) {
			for (auto& [id, resource] : typeResources) {
				resource->unload(); // ensure all resources clean up properly
			}
			typeResources.clear(); // clear type specific container
		}
		_refCounts.clear(); // reset all reference counts
		_resources.clear(); // clear resource map
	}


private:
	static constexpr uint32_t MAX_BINDLESS_TEXTURES = 10000;

	VulkanCore* _vulkanCore{ nullptr };

	VkDescriptorSetLayout _bindlessLayout{ VK_NULL_HANDLE };
	VkDescriptorPool _bindlessPool{ VK_NULL_HANDLE };
	VkDescriptorSet _bindlessSet{ VK_NULL_HANDLE };
	std::vector<uint32_t> _freeBindlessSlots;
	uint32_t _nextBindlessSlot{ 0 }; // 0 for invalid
	std::mutex _bindlessMutex;

	// two level storage system
	// organize by type first, then by unique id
	std::unordered_map<std::type_index, std::unordered_map<std::string, std::shared_ptr<Resource>>> _resources;

	std::unordered_map<std::type_index, std::unordered_map<std::string, ResourceData>> _refCounts;

	void init_bindless();
	void clean_bindless();
};

template<typename T>
inline ResourceHandle<T>::ResourceHandle()
	: resourceManager(nullptr) {}

template<typename T>
inline ResourceHandle<T>::ResourceHandle(const std::string& id, ResourceManager* manager)
	: resourceId(id), resourceManager(manager) {}

template<typename T>
inline T* ResourceHandle<T>::get() const
{
	if (!resourceManager)
		return nullptr;

	return resourceManager->get_resource<T>(resourceId);
}

template<typename T>
inline bool ResourceHandle<T>::is_valid() const
{
	return resourceManager && resourceManager->has_resource<T>(resourceId);
}

template<typename T>
inline T* ResourceHandle<T>::operator->() const 
{ 
	T* ptr = get();
	assert(ptr != nullptr && "ResourceHandle: attempted to dereference an invalid or unloaded ResourceHandle");
	return ptr; 
}

template<typename T>
inline T& ResourceHandle<T>::operator*() const { return *get(); }

template<typename T>
inline ResourceHandle<T>::operator bool() const { return is_valid(); }