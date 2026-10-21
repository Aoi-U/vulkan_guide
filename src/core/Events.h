#pragma once

#include <core/UUID.h>
#include <vulkan/vulkan.h>
#include <SDL_events.h>
#include <filesystem>

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

	struct SceneSaveEvent
	{
		std::filesystem::path filePath;
	};
	
	struct SceneSaveAsEvent{};

	struct SceneLoadEvent {
		std::filesystem::path filePath;
	};

	struct SceneNewSceneEvent {};

	struct AssetReloadedEvent
	{
		UUID uuid{};
		std::filesystem::path path;
	};

}