#pragma once

#include "app/editor/EditorPanel.h"

class InspectorPanel : public EditorPanel
{
public:
	InspectorPanel();
	void on_draw(EditorContext& ctx) override;
	const char* get_name() const override { return "Inspector"; }

private:
	void register_engine_components();
	bool _lockScale{ true };
};