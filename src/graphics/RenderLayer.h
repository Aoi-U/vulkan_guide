#pragma once

#include <core/layer/ILayer.h>

class RenderLayer : public ILayer
{
public:
	RenderLayer() :
		ILayer("RenderLayer") {}
	~RenderLayer() override = default;

	void on_attach(EngineContext& ctx) override;

	void on_update(float deltaTime) override;
	void on_end_frame() override;
};