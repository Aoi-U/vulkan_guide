#pragma once

#include "app/editor/EditorPanel.h"
#include <imgui.h>

class ViewportPanel : public EditorPanel
{
public:
	void on_draw(EditorContext& ctx) override;
	const char* get_name() const override { return "Viewport"; }

	// getters
	bool is_hovered() const { return _isHovered; }
	bool is_focused() const { return _isFocused; }
	ImVec2 get_viewport_pos() const { return _viewportPos; }
	ImVec2 get_viewport_size() const { return _viewportSize; }

private:
	bool _isHovered{ false };
	bool _isFocused{ false };
	ImVec2 _viewportPos{ 0.0f, 0.0f };
	ImVec2 _viewportSize{ 0.0f, 0.0f };
};