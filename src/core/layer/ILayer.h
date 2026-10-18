#pragma once

#include <core/layer/EngineContext.h>
#include <SDL_events.h>
#include <string>
#include <string_view>
#include <cstdint>

struct EngineEvent
{
	SDL_Event sdlEvent{};
	bool handled{ false };
	bool wantCaptureMouse{ false };
	bool wantCaptureKeyboard{ false };

	bool is_mouse_event() const
	{
		switch (sdlEvent.type) {
		case SDL_MOUSEMOTION:
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
		case SDL_MOUSEWHEEL:
			return true;
		default:
			return false;
		}
	}

	bool is_keyboard_event() const noexcept
	{
		return sdlEvent.type == SDL_KEYDOWN ||
			sdlEvent.type == SDL_KEYUP ||
			sdlEvent.type == SDL_TEXTINPUT;
	}

	bool is_window_event() const noexcept
	{
		return sdlEvent.type == SDL_WINDOWEVENT || sdlEvent.type == SDL_QUIT;
	}
};

class ILayer
{
public:
	explicit ILayer(std::string name, bool enabledByDefault = true)
		: _name(std::move(name)), _enabled(enabledByDefault) {}

	virtual ~ILayer() = default;

	ILayer(const ILayer&) = delete;
	ILayer& operator=(const ILayer&) = delete;
	ILayer(ILayer&&) noexcept = default;
	ILayer& operator=(ILayer&&) noexcept = default;

	virtual void on_attach(EngineContext& ctx) { _ctx = &ctx; } // called when the layer is pushed onto the layerstack
	virtual void on_detach() { _ctx = nullptr; } // called when the layer is removed or layerstack is destroyed

	virtual void on_begin_frame(uint64_t frameNumber) {} // called at the start of each frame
	virtual void on_fixed_update(float fixedDeltaTime) {} // caled in a fixed timestep
	virtual void on_update(float deltaTime) {} // called once per frame
	virtual void on_imgui_render() {} // optional hook for layers to draw debug imgui
	virtual void on_end_frame() {} // called at the end of each frame

	virtual void on_event(EngineEvent& event) {} // called for every polled os/input event

	const std::string& get_name() const noexcept { return _name; }
	bool is_enabled() const noexcept { return _enabled; }
	void set_enabled(bool enabled) noexcept { _enabled = enabled; }

protected:
	std::string _name;
	bool _enabled{ true };
	EngineContext* _ctx{ nullptr };
};