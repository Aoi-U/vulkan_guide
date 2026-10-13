#pragma once

#include <app/editor/EditorContext.h>
#include <string>

// base class for all editor panels
class EditorPanel
{
public:
	virtual ~EditorPanel() = default;
	virtual void on_draw(EditorContext& ctx) = 0;
	virtual const char* get_name() const = 0;

	bool is_open{ true };
};