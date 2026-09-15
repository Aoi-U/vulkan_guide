#include "App.h"
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <chrono>
#include <thread>

App::App()
{
	_window = std::make_unique<Window>("Vulkan Engine", 1280, 720);
	_graphicsEngine = std::make_unique<VulkanEngine>();
}

void App::init()
{
	_graphicsEngine->init(_window.get());
}

void App::run()
{
	auto lastTime = std::chrono::steady_clock::now();

	while (!_window->should_close()) {
		auto currentTime = std::chrono::steady_clock::now();
		float deltaTime = std::chrono::duration<float>(currentTime - lastTime).count();
		lastTime = currentTime;

		_graphicsEngine->stats.deltaTime = deltaTime;
		_graphicsEngine->stats.frameTime = deltaTime * 1000.f;

		_window->poll_events([&](const SDL_Event& e) {
			const ImGuiIO& io = ImGui::GetIO();
			if (!io.WantCaptureMouse || _graphicsEngine->mainCamera.isActive) {
				_graphicsEngine->mainCamera.processSDLEvent(const_cast<SDL_Event&>(e));
			}
			});

		// skip rendering when the window is minimized
		if (_window->is_minimized()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}

		// resize swapchain if window has been resized
		if (_window->is_resized()) {
			_graphicsEngine->resize_swapchain();
			_window->reset_resized_flag();
		}

		// render a frame
		_graphicsEngine->render_frame();

	}
}

void App::cleanup()
{
	_graphicsEngine->cleanup();
	_window->cleanup();
}
