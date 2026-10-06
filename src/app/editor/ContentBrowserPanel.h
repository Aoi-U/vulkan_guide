#pragma once

#include "app/editor/EditorPanel.h"
#include "EditorContext.h"
#include <filesystem>

class ContentBrowserPanel : public EditorPanel
{
public:
	ContentBrowserPanel();
	void on_draw(EditorContext& ctx) override;
	const char* get_name() const override { return "Content Browser"; }

private:
	std::filesystem::path _baseDirectory;
	std::filesystem::path _currentDirectory;
};