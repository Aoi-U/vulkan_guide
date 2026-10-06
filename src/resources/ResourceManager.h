#pragma once

#include <string>
#include <optional>
#include <unordered_map>
#include <memory>
#include <typeindex>
#include <vector>
#include <type_traits>
#include <utility>
#include <typeinfo>

class VulkanCore;
class ResourceManager;

// handle to a resource, managed by resource manager
template<typename T>
class ResourceHandle
{
public:
	ResourceHandle()
		: resourceManager(nullptr) {}

	ResourceHandle(const std::string& id, ResourceManager* manager)
		: resourceId(id), resourceManager(manager) {}

	// Declarations only
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
	explicit Resource(const std::string& id)
		: resourceId(id) {}

	virtual ~Resource() = default;

	const std::string& get_id() const { return resourceId; }
	bool is_loaded() const { return loaded; }

	bool load(VulkanCore* core)
	{
		if (loaded)
			return true;

		loaded = doLoad(core);
		return loaded;
	}

	void unload(VulkanCore* core)
	{
		if (!loaded)
			return;

		doUnload(core);
		loaded = false;
	}

protected:
	virtual bool doLoad(VulkanCore* core) = 0;
	virtual bool doUnload(VulkanCore* core) = 0;

private:
	std::string resourceId;
	bool loaded = false;
};

// resource manager
class ResourceManager
{
public:
	ResourceManager() = default;
	explicit ResourceManager(VulkanCore* vulkanCore)
		: _vulkanCore(vulkanCore) {}

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
		auto resource = std::make_shared<T>(resourceId, std::forward<Args>(args)...);
		if (!resource->load(_vulkanCore)) {
			return ResourceHandle<T>(); // return invalid handle if loading fails
		}

		// cache successful resource and initialize reference tracking
		typeResources[resourceId] = resource;
		_refCounts[typeKey][resourceId] = ResourceData{ resource, 1 };

		return ResourceHandle<T>(resourceId, this);
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
		if (it->second.refCount <= 0) {
			auto resTypeIt = _resources.find(typeKey);

			if (resTypeIt == _resources.end())
				return;

			auto& typeResources = resTypeIt->second;
			auto resourceIt = typeResources.find(resourceId);

			if (resourceIt == typeResources.end())
				return;

			resourceIt->second->unload(_vulkanCore); // allow resource to clean up its data
			typeResources.erase(resourceIt); // remove from cache
		}

		typeIt->second.erase(it); // clean up reference counting entry
	}

	void unload_all()
	{
		for (auto& [type, typeResources] : _resources) {
			for (auto& [id, resource] : typeResources) {
				resource->unload(_vulkanCore); // ensure all resources clean up properly
			}
			typeResources.clear(); // clear type specific container
		}
		_refCounts.clear(); // reset all reference counts
		_resources.clear(); // clear resource map
	}


private:
	VulkanCore* _vulkanCore;
	// two level storage system
	// organize by type first, then by unique id
	std::unordered_map<std::type_index, std::unordered_map<std::string, std::shared_ptr<Resource>>> _resources;

	// two level ref count for automatic resource lifecycle management
	// maps resource type first, then resource id to their data
	struct ResourceData
	{
		std::shared_ptr<Resource> resource; // the actual resource
		int refCount; // reference count for this resource
	};
	std::unordered_map<std::type_index, std::unordered_map<std::string, ResourceData>> _refCounts;
};

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
inline T* ResourceHandle<T>::operator->() const { return get(); }

template<typename T>
inline T& ResourceHandle<T>::operator*() const { return *get(); }

template<typename T>
inline ResourceHandle<T>::operator bool() const { return is_valid(); }