#pragma once

#include "app/editor/EditorPanel.h"

class PerformancePanel : public EditorPanel
{
public:
	void on_draw(EditorContext& ctx) override;
	const char* get_name() const override { return "Performance"; }
};