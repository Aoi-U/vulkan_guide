#pragma once

#include <vulkan/vulkan.h>
#include <string>
#include <functional>
#include <SDL_events.h>
#include <entt.hpp>

struct SDL_Window;

class Window
{
public:
	void init(const std::string& name, uint32_t width, uint32_t height);
	void cleanup();

	// create a vulkan surface for the window
	bool create_surface(VkInstance instance, VkSurfaceKHR* surface);


	void poll_events(entt::dispatcher& dispatcher);

	bool should_close() const { return _shouldClose; }
	bool is_minimized() const { return _isMinimized; }
	VkExtent2D get_extent() const { return _extent; }
	SDL_Window* get_sdl_window() const { return _window; }

private:
	struct SDL_Window* _window{ nullptr };
	VkExtent2D _extent;
	bool _shouldClose{ false };
	bool _isMinimized{ false };
	bool _isResized{ false };

};