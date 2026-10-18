#include "LayerStack.h"

LayerStack::~LayerStack()
{
	if (_eventDispatcher) {
		disconnect_dispatcher(*_eventDispatcher);
	}
	clear();
}

ILayer* LayerStack::get_layer(std::string_view name) const
{
	for (const auto& layer : _layers) {
		if (layer->get_name() == name)
			return layer.get();
	}
	return nullptr;
}

bool LayerStack::pop_layer(ILayer* layer)
{
	if (!layer)
		return false;

	auto endIt = _layers.begin() + static_cast<std::ptrdiff_t>(_layerInsertIndex);
	auto it = std::find_if(_layers.begin(), endIt, [layer](const std::unique_ptr<ILayer>& ptr) {
		return ptr.get() == layer;
		});

	if (it != endIt) {
		(*it)->on_detach();
		_layers.erase(it);
		_layerInsertIndex--;
		return true;
	}
	return false;
}

bool LayerStack::pop_overlay(ILayer* overlay)
{
	if (!overlay)
		return false;

	auto startIt = _layers.begin() + static_cast<std::ptrdiff_t>(_layerInsertIndex);
	auto it = std::find_if(startIt, _layers.end(), [overlay](const std::unique_ptr<ILayer>& ptr) {
		return ptr.get() == overlay;
		});

	if (it != _layers.end()) {
		(*it)->on_detach();
		_layers.erase(it);
		return true;
	}
	return false;
}

void LayerStack::clear()
{
	// detach layers in reverse order
	for (auto it = _layers.rbegin(); it != _layers.rend(); it++) {
		if (*it) {
			(*it)->on_detach();
		}
	}
	_layers.clear();
	_layerInsertIndex = 0;
}

void LayerStack::connect_dispatcher(entt::dispatcher & dispatcher)
{
	if (_eventDispatcher) {
		disconnect_dispatcher(*_eventDispatcher);
	}

	_eventDispatcher = &dispatcher;
	_eventDispatcher->sink<Events::SDLInputEvent>().connect<&LayerStack::on_sdl_input_event>(this);
}

void LayerStack::disconnect_dispatcher(entt::dispatcher & dispatcher)
{
	dispatcher.sink<Events::SDLInputEvent>().disconnect(this);
	if (_eventDispatcher == &dispatcher) {
		_eventDispatcher = nullptr;
	}
}

void LayerStack::dispatch_event(EngineEvent & event)
{
	// dispatch events top to bottom
	for (auto it = _layers.rbegin(); it != _layers.rend(); it++) {
		if (event.handled) {
			break;
		}
		if ((*it)->is_enabled()) {
			(*it)->on_event(event);
		}
	}
}

void LayerStack::begin_frame(uint64_t frameNumber)
{
	for (const auto& layer : _layers) {
		if (layer->is_enabled()) {
			layer->on_begin_frame(frameNumber);
		}
	}
}

void LayerStack::fixed_update(float fixedDeltaTime)
{
	for (const auto& layer : _layers) {
		if (layer->is_enabled()) {
			layer->on_fixed_update(fixedDeltaTime);
		}
	}
}

void LayerStack::update(float deltaTime)
{
	for (const auto& layer : _layers) {
		if (layer->is_enabled()) {
			layer->on_update(deltaTime);
		}
	}
}

void LayerStack::render_imgui()
{
	for (const auto& layer : _layers) {
		if (layer->is_enabled()) {
			layer->on_imgui_render();
		}
	}
}

void LayerStack::end_frame()
{
	for (const auto& layer : _layers) {
		if (layer->is_enabled()) {
			layer->on_end_frame();
		}
	}
}

void LayerStack::on_sdl_input_event(const Events::SDLInputEvent & e)
{
	EngineEvent event{};
	event.sdlEvent = e.raw;
	dispatch_event(event);
}
