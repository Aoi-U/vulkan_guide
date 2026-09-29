#pragma once
#include <vulkan/vulkan.h>
#include <SDL_events.h>
#include <entt.hpp>
#include <memory>
#include <string>

namespace Events
{
	// window events
	struct WindowResizeEvent
	{
		uint32_t width;
		uint32_t height;
	};

	struct WindowCloseEvent {};

	struct WindowMinimizeEvent {
		bool minimized;
	};

	// input event
	struct SDLInputEvent {
		SDL_Event raw;
	};

	// entity / asset events
	struct EntitySelectedEvent {
		entt::entity entity{ entt::null };
	};

	struct EntityDestroyedEvent {
		entt::entity entity{ entt::null };
	};

	struct SceneLoadedEvent {
		std::string name;
	};
}