#pragma once

#include "app/editor/EditorPanel.h"

class CVarPanel : public EditorPanel
{
public:
	void on_draw(EditorContext& ctx) override;
	const char* get_name() const override { return "Console Variables"; }
};