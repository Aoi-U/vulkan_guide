#pragma once

#include "app/editor/EditorPanel.h"

class EnvironmentPanel : public EditorPanel
{
public:
	void on_draw(EditorContext& ctx) override;
	const char* get_name() const override { return "Environment"; }

private:
	std::array<char, 260> _hdrPathBuffer{ "../../assets/cubemaps/royal_esplanade_4k.hdr" };
	std::string _currentEnvLabel{ "Procedural sky" };
	std::string _statusMsg{ "" };
	bool _isError{ false };
};