#include "window.h"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <imgui_impl_sdl2.h>
#include <core/Events.h>

void Window::init(const std::string& name, uint32_t width, uint32_t height)
{

	// We initialize SDL and create a window with it
	SDL_Init(SDL_INIT_VIDEO);

	SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
	_window = SDL_CreateWindow(
		name.c_str(),
		SDL_WINDOWPOS_UNDEFINED,
		SDL_WINDOWPOS_UNDEFINED,
		width,
		height,
		window_flags
	);

	_extent.width = width;
	_extent.height = height;
}

void Window::cleanup()
{
	if (_window) {
		SDL_DestroyWindow(_window);
		_window = nullptr;
	}
	SDL_Quit();
}

void Window::poll_events(entt::dispatcher& dispatcher)
{
	SDL_Event e;
	while (SDL_PollEvent(&e) != 0) {
		// forward raw input event to subscribers
		dispatcher.trigger<Events::SDLInputEvent>({ e });

		if (e.type == SDL_QUIT) {
			_shouldClose = true;
			dispatcher.trigger<Events::WindowCloseEvent>({});
		}
		else if (e.type == SDL_WINDOWEVENT) {
			if (e.window.event == SDL_WINDOWEVENT_MINIMIZED) {
				_isMinimized = true;
				dispatcher.trigger<Events::WindowMinimizeEvent>({ true });
			}
			else if (e.window.event == SDL_WINDOWEVENT_RESTORED) {
				_isMinimized = false;
				dispatcher.trigger<Events::WindowMinimizeEvent>({ false });
			}
			else if (e.window.event == SDL_WINDOWEVENT_RESIZED) {
				int w, h;
				SDL_GetWindowSize(_window, &w, &h);
				_extent.width = static_cast<uint32_t>(w);
				_extent.height = static_cast<uint32_t>(h);

				dispatcher.trigger<Events::WindowResizeEvent>({ _extent.width, _extent.height });
			}
		}
	}
}

bool Window::create_surface(VkInstance instance, VkSurfaceKHR* surface) {
	return SDL_Vulkan_CreateSurface(_window, instance, surface) == SDL_TRUE;
}

void Window::set_title(const std::string& title)
{
	if (_window)
	{
		SDL_SetWindowTitle(_window, title.c_str());
	}
}
