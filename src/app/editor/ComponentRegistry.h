#pragma once

#include <entt.hpp>
#include <imgui.h>
#include <string>
#include <functional>
#include <vector>
#include <app/editor/EditorContext.h>

class ComponentRegistry
{
public:
	struct Entry {
		std::string name;
		bool removable{ true };
		std::function<bool(entt::registry&, entt::entity)> has;
		std::function<void(entt::registry&, entt::entity)> add;
		std::function<void(entt::registry&, entt::entity)> remove;
		std::function<void(entt::registry&, entt::entity, EditorContext&)> draw;
	};

	static ComponentRegistry& get()
	{
		static ComponentRegistry instance;
		return instance;
	}

	template<typename T, typename DrawFunc>
	void register_component(std::string name, DrawFunc drawFunc, bool removable = true)
	{
		Entry entry{
			.name = std::move(name),
			.removable = removable,
			.has = [](entt::registry& r, entt::entity e) {
				return r.all_of<T>(e);
			},
			.add = [](entt::registry& r, entt::entity e) {
				r.emplace<T>(e);
			},
			.remove = [](entt::registry& r, entt::entity e) {
				r.remove<T>(e);
			},
			.draw = [drawFunc](entt::registry& r, entt::entity e, EditorContext& ctx) {
				drawFunc(r.get<T>(e), ctx);
			}
		};

		_entries.push_back(std::move(entry));
	}

	const std::vector<Entry>& get_entries() const { return _entries; }

private:
	std::vector<Entry> _entries;
};

