#pragma once

#include <vulkan/vulkan.h>
#include <string>
#include <functional>
#include <SDL_events.h>

struct SDL_Window;

class Window
{
public:
	void init(const std::string& name, uint32_t width, uint32_t height);
	void cleanup();

	// create a vulkan surface for the window
	bool create_surface(VkInstance instance, VkSurfaceKHR* surface);

	// process SDL events 
	void poll_events(const std::function<void(const SDL_Event&)>& eventCallback = nullptr);

	void reset_resized_flag() { _isResized = false; }

	bool should_close() const { return _shouldClose; }
	bool is_minimized() const { return _isMinimized; }
	bool is_resized() const { return _isResized; }
	VkExtent2D get_extent() const { return _extent; }
	SDL_Window* get_sdl_window() const { return _window; }

private:
	struct SDL_Window* _window{ nullptr };
	VkExtent2D _extent;
	bool _shouldClose{ false };
	bool _isMinimized{ false };
	bool _isResized{ false };

};