#pragma once

#include <entt.hpp>
#include <yaml-cpp/yaml.h>
#include <functional>
#include <string>
#include <vector>

class ResourceManager;

struct ComponentRegistryEntry
{
	std::string name;
	std::function<bool(const entt::registry&, entt::entity)> hasComponent;
	std::function<void(entt::registry&, entt::entity)> addComponent;
	std::function<void(entt::registry&, entt::entity)> removeComponent;
	std::function<void(const entt::registry&, entt::entity, YAML::Emitter&)> serialize;
	std::function<void(entt::registry&, entt::entity, const YAML::Node&, ResourceManager*)> deserialize;
};

// manages the registration of components and their yaml serialization/deserialization functions
class ComponentRegistry
{
public:
	static ComponentRegistry& get()
	{
		static ComponentRegistry instance;
		return instance;
	}

	void init_default_components();
	void register_component(ComponentRegistryEntry entry);

	const std::vector<ComponentRegistryEntry>& get_entries() const { return _entries; }

private:
	bool _initialized{ false };
	std::vector<ComponentRegistryEntry> _entries;
};
