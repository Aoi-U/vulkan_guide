#pragma once

#include <core/UUID.h>
#include <resources/AssetRegistry.h>
#include <graphics/utils/StagingUploader.h>
#include <graphics/reflection/PipelineLayoutCache.h>
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
#include <deque>
#include <functional>

// resource management classes heavily based off vulkan resource management tutorial: 
// https://docs.vulkan.org/tutorial/latest/Building_a_Simple_Engine/Engine_Architecture/04_resource_management.html#_resource_management

class VulkanCore;
class ResourceManager;
class GlobalGeometryPool;
class AssetStreamer;
class VirtualTextureSystem;

// handle to a resource, managed by resource manager
template<typename T>
class ResourceHandle
{
public:
	ResourceHandle();
	ResourceHandle(const UUID& uuid, std::string_view debugId, ResourceManager* manager);
	ResourceHandle(const UUID& uuid, ResourceManager* manager);
	ResourceHandle(const std::string& id, ResourceManager* manager);

	ResourceHandle(const ResourceHandle& other);
	ResourceHandle& operator=(const ResourceHandle& other);
	ResourceHandle(ResourceHandle&& other) noexcept;
	ResourceHandle& operator=(ResourceHandle&& other) noexcept;
	~ResourceHandle();

	T* get() const;
	bool is_valid() const;
	const UUID& get_uuid() const { return resourceUuid; }
	const std::string& get_id() const { return resourceId; }

	// Declarations only
	T* operator->() const;
	T& operator*() const;
	operator bool() const;

private:
	UUID resourceUuid{};
	std::string resourceId;
	ResourceManager* resourceManager;
};

template<typename T>
using AssetHandle = ResourceHandle<T>;

// resource base class
class Resource
{
public:
	explicit Resource(const std::string& id, VulkanCore* core, ResourceManager* manager)
		: resourceUuid(AssetRegistry::get().resolve_or_derive_uuid(id)), resourceId(id), _core(core), _resourceManager(manager) {}

	explicit Resource(const UUID& uuid, const std::string& id, VulkanCore* core, ResourceManager* manager)
		: resourceUuid(uuid), resourceId(id), _core(core), _resourceManager(manager) {}

	virtual ~Resource() = default;

	const UUID& get_uuid() const { return resourceUuid; }
	void set_uuid(const UUID& uuid) { resourceUuid = uuid; }
	const std::string& get_id() const { return resourceId; }
	bool is_loaded() const { return loaded; }

	bool load()
	{
		if (loaded) 
			return true;

		loaded = doLoad();
		if (loaded && resourceUuid.is_valid()) {
			AssetRegistry::get().set_resource_state(resourceUuid, ResourceState::Loaded);
		}
		else if (!loaded && resourceUuid.is_valid()) {
			AssetRegistry::get().set_resource_state(resourceUuid, ResourceState::Failed);
		}
		return loaded;
	}

	void unload()
	{
		if (!loaded) 
			return;

		doUnload();
		loaded = false;
		if (resourceUuid.is_valid()) {
			AssetRegistry::get().set_resource_state(resourceUuid, ResourceState::Unloaded);
			AssetRegistry::get().set_gpu_size(resourceUuid, 0);
		}
	}

protected:
	virtual bool doLoad() = 0;
	virtual bool doUnload() = 0;

	Resource() = default;
	Resource(Resource&& other) noexcept
		: resourceUuid(other.resourceUuid), resourceId(std::move(other.resourceId)), loaded(other.loaded), _core(other._core), _resourceManager(other._resourceManager)
	{
		other.resourceUuid = UUID::null();
		other.loaded = false;
		other._core = nullptr;
		other._resourceManager = nullptr;
	}

	Resource& operator=(Resource&& other) noexcept
	{
		if (this != &other) {
			resourceUuid = other.resourceUuid;
			resourceId = std::move(other.resourceId);
			loaded = other.loaded;
			_core = other._core;
			_resourceManager = other._resourceManager;
			other.resourceUuid = UUID::null();
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
	UUID resourceUuid{};
	std::string resourceId;
	bool loaded = false;

};

// resource manager
class ResourceManager
{
public:
	static constexpr uint64_t EVICTION_FRAME_THRESHOLD = 60; // number of frames after which unused resources can be evicted
	// two level ref count for automatic resource lifecycle management
	// maps resource type first, then resource id to their data
	struct ResourceData
	{
		std::shared_ptr<Resource> resource; // the actual resource
		int refCount; // reference count for this resource
		uint64_t lastAccessedFrame{ 0 }; // frame index when this resource was last accessed
	};

	ResourceManager();
	~ResourceManager();
	
	void init(VulkanCore* core);

	void cleanup();

	uint32_t allocate_bindless_texture(VkImageView view, VkSampler sampler);
	void free_bindless_slot(uint32_t slot);

	// register multiple textures in a single batch to reduce mutex and descriptor set update overhead
	// outSlots: vector to store the allocated bindless slots for each texture
	void register_bindless_texture_batched(const std::vector<std::pair<VkImageView, VkSampler>>& textures, std::vector<uint32_t>& outSlots); 

	void queue_main_thread_task(std::function<void()> task);
	void process_main_thread_tasks();

	// unloads zero-ref resources past EVICTION_FRAME_THRESHOLD or over vram limit
	void process_evictions(uint64_t currentFrame);
	void process_evictions_immediate();
	void request_async_load(const UUID& uuid);

	VkDescriptorSetLayout get_bindless_layout() const { return _bindlessLayout; }
	VkDescriptorSet get_bindless_set() const { return _bindlessSet; }
	GlobalGeometryPool& get_geometry_pool() { return *_geometryPool; }
	PipelineLayoutCache& get_pipeline_layout_cache() { return _layoutCache; }
	StagingUploader& get_staging_uploader() { return *_stagingUploader; }
	VirtualTextureSystem& get_vtsystem() { return *_vtSystem; }
	AssetStreamer& get_asset_streamer() { return *_assetStreamer; }

	template<typename T, typename... Args>
	ResourceHandle<T> load_by_uuid(const UUID& uuid, const std::string& debugName, Args&&... args)
	{
		static_assert(std::is_base_of_v<Resource, T>, "T must derive from Resource");
		if (!uuid.is_valid()) return ResourceHandle<T>();

		auto typeKey = std::type_index(typeid(T));
		auto& typeResources = _resources[typeKey];
		auto it = typeResources.find(uuid);
		if (it != typeResources.end()) {
			return ResourceHandle<T>(uuid, debugName, this);
		}

		auto resource = std::make_shared<T>(debugName, _vulkanCore, this, std::forward<Args>(args)...);
		resource->set_uuid(uuid);
		if (!resource->load()) {
			return ResourceHandle<T>();
		}

		typeResources[uuid] = resource;
		_refCounts[typeKey][uuid] = ResourceData{ resource, 0, _currentFrame };
		return ResourceHandle<T>(uuid, debugName, this);
	}

	template<typename T, typename... Args> // string based load resolving UUID first
	ResourceHandle<T> load(const std::string& resourceId, Args&&... args)
	{
		UUID uuid = AssetRegistry::get().resolve_or_derive_uuid(resourceId);
		return load_by_uuid<T>(uuid, resourceId, std::forward<Args>(args)...);
	}

	template<typename T>
	T* get_or_request(const UUID& uuid, T* fallback = nullptr)
	{
		if (!uuid.is_valid()) 
			return fallback;
		if (T* res = get_resource<T>(uuid)) 
			return res;
		
		ResourceState currentState = AssetRegistry::get().get_resource_state(uuid);
		if (currentState == ResourceState::Unloaded)
			request_async_load(uuid);
		
		return fallback;
	}

	template<typename T>
	T* get_resource(const UUID& uuid) // resource by uuid
	{
		auto& typeResources = _resources[std::type_index(typeid(T))];
		auto it = typeResources.find(uuid);
		if (it != typeResources.end()) {
			_refCounts[std::type_index(typeid(T))][uuid].lastAccessedFrame = _currentFrame;

			return static_cast<T*>(it->second.get());
		}
		return nullptr;
	}

	template<typename T>
	T* get_resource(const std::string& resourceId) // resource by resourceid
	{
		return get_resource<T>(AssetRegistry::get().resolve_or_derive_uuid(resourceId));
	}

	template<typename T>
	ResourceHandle<T> get_handle(const UUID& uuid) // handle by uuid
	{
		static_assert(std::is_base_of_v<Resource, T>, "T must derive from Resource");
		if (has_resource<T>(uuid)) {
			const auto* meta = AssetRegistry::get().get_metadata(uuid);
			std::string debugId = meta ? meta->name : uuid.to_string();
			return ResourceHandle<T>(uuid, debugId, this);
		}
		return ResourceHandle<T>();
	}

	template<typename T>
	ResourceHandle<T> get_handle(const std::string& resourceId) // handle by resourceId
	{
		static_assert(std::is_base_of_v<Resource, T>, "T must derive from Resource");
		UUID uuid = AssetRegistry::get().resolve_or_derive_uuid(resourceId);
		if (has_resource<T>(uuid)) {
			return ResourceHandle<T>(uuid, resourceId, this);
		}
		return ResourceHandle<T>();
	}

	template<typename T>
	std::vector<UUID> get_loaded_resource_uuids() const
	{
		std::vector<UUID> keys;
		auto typeKey = std::type_index(typeid(T));
		auto it = _resources.find(typeKey);
		if (it != _resources.end()) {
			for (const auto& [uuid, _] : it->second) {
				keys.push_back(uuid);
			}
		}
		return keys;
	}

	template<typename T>
	std::vector<std::string> get_loaded_resources() const
	{
		std::vector<std::string> keys;
		auto typeKey = std::type_index(typeid(T));
		auto it = _resources.find(typeKey);
		if (it != _resources.end()) {
			for (const auto& [_, res] : it->second) {
				keys.push_back(res->get_id());
			}
		}
		return keys;
	}

	template<typename T>
	bool has_resource(const UUID& uuid) const
	{
		auto resourceTypeIt = _resources.find(std::type_index(typeid(T)));
		if (resourceTypeIt != _resources.end()) {
			return resourceTypeIt->second.contains(uuid);
		}
		return false;
	}

	template<typename T>
	bool has_resource(const std::string& resourceId) const
	{
		return has_resource<T>(AssetRegistry::get().resolve_or_derive_uuid(resourceId));
	}

	template<typename T>
	void add_ref(const UUID& uuid)
	{
		auto typeKey = std::type_index(typeid(T));
		auto typeIt = _refCounts.find(typeKey);
		if (typeIt != _refCounts.end()) {
			auto it = typeIt->second.find(uuid);
			if (it != typeIt->second.end()) { // increment ref count and update last accessed frame
				it->second.refCount++;
				it->second.lastAccessedFrame = _currentFrame;

				std::erase_if(_evictionCandidates, [&](const EvictionCandidate& candidate) {
					return candidate.typeKey == typeKey && candidate.uuid == uuid;
					});
			}
		}
	}

	template<typename T>
	void release(const UUID& uuid)
	{
		auto typeKey = std::type_index(typeid(T));
		auto typeIt = _refCounts.find(typeKey);
		if (typeIt == _refCounts.end()) 
			return;

		auto it = typeIt->second.find(uuid);
		if (it == typeIt->second.end()) 
			return;

		it->second.refCount--;
		if (it->second.refCount <= 0) { // no more references, unload resource
			it->second.refCount = 0;
			_evictionCandidates.emplace_back(typeKey, uuid, _currentFrame);
		}
	}

	template<typename T>
	void force_unload(const UUID& uuid)
	{
		auto typeKey = std::type_index(typeid(T));
		if (_resources.contains(typeKey) && _resources[typeKey].contains(uuid)) {
			_resources[typeKey][uuid]->unload();
			_resources[typeKey].erase(uuid);
			_refCounts[typeKey].erase(uuid);
		}
	}

	template<typename T>
	void force_unload(const std::string& resourceId)
	{
		force_unload<T>(AssetRegistry::get().resolve_or_derive_uuid(resourceId));
	}

	void unload_all();

private:
	struct EvictionCandidate
	{
		std::type_index typeKey;
		UUID uuid;
		uint64_t zeroRefFrame{ 0 };
	};
	static constexpr uint32_t MAX_BINDLESS_TEXTURES = 10000;

	VulkanCore* _vulkanCore{ nullptr };

	VkDescriptorSetLayout _bindlessLayout{ VK_NULL_HANDLE };
	VkDescriptorPool _bindlessPool{ VK_NULL_HANDLE };
	VkDescriptorSet _bindlessSet{ VK_NULL_HANDLE };

	std::vector<uint32_t> _freeBindlessSlots;
	uint32_t _nextBindlessSlot{ 0 }; // 0 for invalid
	std::mutex _bindlessMutex;

	std::mutex _taskMutex;
	std::deque<std::function<void()>> _mainThreadTasks;

	std::unique_ptr<GlobalGeometryPool> _geometryPool;
	std::unique_ptr<VirtualTextureSystem> _vtSystem;
	PipelineLayoutCache _layoutCache;
	uint64_t _currentFrame{ 0 };

	std::unique_ptr<StagingUploader> _stagingUploader;

	std::unique_ptr<AssetStreamer> _assetStreamer;
	std::vector<EvictionCandidate> _evictionCandidates;

	// two level storage system
	// organize by type first, then by unique id
	std::unordered_map<std::type_index, std::unordered_map<UUID, std::shared_ptr<Resource>>> _resources;

	std::unordered_map<std::type_index, std::unordered_map<UUID, ResourceData>> _refCounts;

	void init_bindless();
	void clean_bindless();
	void queue_unload(std::shared_ptr<Resource> resource);
};

template<typename T>
inline ResourceHandle<T>::ResourceHandle() 
	: resourceManager(nullptr) {}

template<typename T>
inline ResourceHandle<T>::ResourceHandle(const UUID& uuid, std::string_view debugId, ResourceManager* manager)
	: resourceUuid(uuid), resourceId(debugId), resourceManager(manager)
{
	if (resourceManager && resourceUuid.is_valid()) {
		resourceManager->add_ref<T>(resourceUuid);
	}
}

template<typename T>
inline ResourceHandle<T>::ResourceHandle(const UUID& uuid, ResourceManager* manager)
	: resourceUuid(uuid), resourceId(uuid.to_string()), resourceManager(manager)
{
	if (resourceManager && resourceUuid.is_valid()) {
		resourceManager->add_ref<T>(resourceUuid);
	}
}

template<typename T>
inline ResourceHandle<T>::ResourceHandle(const std::string& id, ResourceManager* manager)
	: resourceUuid(AssetRegistry::get().resolve_or_derive_uuid(id)), resourceId(id), resourceManager(manager)
{
	if (resourceManager && resourceUuid.is_valid()) {
		resourceManager->add_ref<T>(resourceUuid);
	}
}

template<typename T>
inline ResourceHandle<T>::ResourceHandle(const ResourceHandle& other) // copy constructor
	: resourceUuid(other.resourceUuid), resourceId(other.resourceId), resourceManager(other.resourceManager)
{
	if (resourceManager && resourceUuid.is_valid()) {
		resourceManager->add_ref<T>(resourceUuid);
	}
}

template<typename T>
inline ResourceHandle<T>& ResourceHandle<T>::operator=(const ResourceHandle& other) // copy assignment
{
	if (this != &other) {
		if (resourceManager && resourceUuid.is_valid()) {
			resourceManager->release<T>(resourceUuid);
		}
		resourceUuid = other.resourceUuid;
		resourceId = other.resourceId;
		resourceManager = other.resourceManager;
		if (resourceManager && resourceUuid.is_valid()) {
			resourceManager->add_ref<T>(resourceUuid);
		}
	}
	return *this;
}

template<typename T>
inline ResourceHandle<T>::ResourceHandle(ResourceHandle&& other) noexcept // move constructor
	: resourceUuid(other.resourceUuid), resourceId(std::move(other.resourceId)), resourceManager(other.resourceManager)
{
	other.resourceUuid = UUID::null();
	other.resourceManager = nullptr;
}

template<typename T>
inline ResourceHandle<T>& ResourceHandle<T>::operator=(ResourceHandle&& other) noexcept // move assignment
{
	if (this != &other) {
		if (resourceManager && resourceUuid.is_valid()) {
			resourceManager->release<T>(resourceUuid);
		}
		resourceUuid = other.resourceUuid;
		resourceId = std::move(other.resourceId);
		resourceManager = other.resourceManager;
		other.resourceUuid = UUID::null();
		other.resourceManager = nullptr;
	}
	return *this;
}

template<typename T>
inline ResourceHandle<T>::~ResourceHandle() // destructor
{
	if (resourceManager && resourceUuid.is_valid()) {
		resourceManager->release<T>(resourceUuid);
	}
}

template<typename T>
inline T* ResourceHandle<T>::get() const
{
	if (!resourceManager || !resourceUuid.is_valid()) return nullptr;
	return resourceManager->get_resource<T>(resourceUuid);
}

template<typename T>
inline bool ResourceHandle<T>::is_valid() const
{
	return resourceManager && resourceUuid.is_valid() && resourceManager->has_resource<T>(resourceUuid);
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