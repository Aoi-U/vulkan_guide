#pragma once

#include <core/layer/ILayer.h>
#include <core/Events.h>
#include <entt.hpp>
#include <vector>
#include <memory>
#include <string_view>
#include <type_traits>
#include <algorithm>
#include <cstdint>

class LayerStack
{
public:
	LayerStack() = default;
	~LayerStack();

	LayerStack(const LayerStack&) = delete;
	LayerStack& operator=(const LayerStack&) = delete;

	template<typename T, typename... Args>
	T* push_layer(EngineContext& ctx, Args&&... args)
	{
		static_assert(std::is_base_of_v<ILayer, T>, "T must derive from ILayer");
		auto layer = std::make_unique<T>(std::forward<Args>(args)...);
		T* ptr = layer.get();
		_layers.emplace(_layers.begin() + static_cast<std::ptrdiff_t>(_layerInsertIndex), std::move(layer));
		_layerInsertIndex++;
		ptr->on_attach(ctx);
		return ptr;
	}

	template<typename T, typename... Args>
	T* push_overlay(EngineContext& ctx, Args&&... args)
	{
		static_assert(std::is_base_of_v<ILayer, T>, "T must derive from ILayer");
		auto overlay = std::make_unique<T>(std::forward<Args>(args)...);
		T* ptr = overlay.get();
		_layers.emplace_back(std::move(overlay));
		ptr->on_attach(ctx);
		return ptr;
	}

	// finds the first layer matching T
	template<typename T>
	T* get_layer() const 
	{
		static_assert(std::is_base_of_v<ILayer, T>, "T must derive from ILayer");
		for (const auto& layer : _layers) {
			if (auto* casted = dynamic_cast<T*>(layer.get())) {
				return casted;
			}
		}
		return nullptr;
	}

	// finds layer by name
	ILayer* get_layer(std::string_view name) const;

	bool pop_layer(ILayer* layer);
	bool pop_overlay(ILayer* overlay);

	// detach all layers 
	void clear(); 

	void connect_dispatcher(entt::dispatcher& dispatcher);
	void disconnect_dispatcher(entt::dispatcher& dispatcher);
	void dispatch_event(EngineEvent& event);

	void begin_frame(uint64_t frameNumber);
	void fixed_update(float fixedDeltaTime);
	void update(float deltaTime);
	void render_imgui();
	void end_frame();

	size_t size() const noexcept { return _layers.size(); }
	bool empty() const noexcept { return _layers.empty(); }

	auto begin() noexcept { return _layers.begin(); }
	auto end() noexcept { return _layers.end(); }
	auto rbegin() noexcept { return _layers.rbegin(); }
	auto rend() noexcept { return _layers.rend(); }

private:
	void on_sdl_input_event(const Events::SDLInputEvent& e);

	std::vector<std::unique_ptr<ILayer>> _layers;
	size_t _layerInsertIndex{ 0 };
	entt::dispatcher* _eventDispatcher{ nullptr };
};