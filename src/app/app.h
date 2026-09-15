#pragma once
#include <window/window.h>
#include <graphics/vk_engine.h>
#include <memory>

class App
{
public:
	App();
	~App() = default;

	void init();
	void run(); // main loop
	void cleanup();

private:
	std::unique_ptr<Window> _window;
	std::unique_ptr<VulkanEngine> _graphicsEngine;
};