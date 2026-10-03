#pragma once

#include "app/editor/EditorPanel.h"

class HierarchyPanel : public EditorPanel
{
public:
	void on_draw(EditorContext& ctx) override;
	const char* get_name() const override { return "Hierarchy"; }
};