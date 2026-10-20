#include "cvars.h"

#include <algorithm>
#include <array>
#include <imgui.h>

template<typename T>
struct CVarStorage
{
	T initial;
	T current;
	CVarParameter* parameter;
};

class CVarSystemImpl : public CVarSystem
{
public:
	static CVarSystemImpl* Get()
	{
		static CVarSystemImpl impl;
		return &impl;
	}

	CVarParameter* GetCVar(std::string_view name) override
	{
		auto it = cvars.find(std::string(name));
		return (it != cvars.end()) ? &it->second : nullptr;
	}

	double GetFloatCVar(std::string_view name) override
	{
		auto cvar = GetCVar(name);
		if (cvar && cvar->type == CVarType::FLOAT) {
			return floatCVars[cvar->arrayIndex].current;
		}
		return 0.0;
	}

	int32_t GetIntCVar(std::string_view name) override
	{
		auto cvar = GetCVar(name);
		if (cvar && cvar->type == CVarType::INT) {
			return intCVars[cvar->arrayIndex].current;
		}
		return 0;
	}

	const char* GetStringCVar(std::string_view name) override
	{
		auto cvar = GetCVar(name);
		if (cvar && cvar->type == CVarType::STRING) {
			return stringCVars[cvar->arrayIndex].current.c_str();
		}
		return "";
	}

	void SetFloatCVar(std::string_view name, double value) override
	{
		auto cvar = GetCVar(name);
		if (cvar && cvar->type == CVarType::FLOAT) {
			floatCVars[cvar->arrayIndex].current = value;
		}
	}

	void SetIntCVar(std::string_view name, int32_t value) override
	{
		auto cvar = GetCVar(name);
		if (cvar && cvar->type == CVarType::INT) {
			intCVars[cvar->arrayIndex].current = value;
		}
	}

	void SetStringCVar(std::string_view name, const char* value) override
	{
		auto cvar = GetCVar(name);
		if (cvar && cvar->type == CVarType::STRING) {
			stringCVars[cvar->arrayIndex].current = std::string(value);
		}
	}

	CVarParameter* CreateFloatCVar(const char* name, const char* description, double defaultValue, double currentValue, CVarFlags flags = CVarFlags::None, double min = 0.0, double max = 1.0) override
	{
		CVarParameter param;
		param.name = name;
		param.description = description;
		param.type = CVarType::FLOAT;
		param.flags = flags;
		param.min = min;
		param.max = max;
		param.arrayIndex = static_cast<int32_t>(floatCVars.size());

		cvars[name] = param;
		CVarParameter* stored = &cvars[name];
		floatCVars.push_back({ defaultValue, currentValue, stored });
		return stored;
	}

	CVarParameter* CreateIntCVar(const char* name, const char* description, int32_t defaultValue, int32_t currentValue, CVarFlags flags = CVarFlags::None) override
	{
		CVarParameter param;
		param.name = name;
		param.description = description;
		param.type = CVarType::INT;
		param.flags = flags;
		param.arrayIndex = static_cast<int32_t>(intCVars.size());

		cvars[name] = param;
		CVarParameter* stored = &cvars[name];
		intCVars.push_back({ defaultValue, currentValue, stored });
		return stored;
	}

	CVarParameter* CreateStringCVar(const char* name, const char* description, const char* defaultValue, const char* currentValue, CVarFlags flags) override {
		CVarParameter param;
		param.name = name;
		param.description = description;
		param.type = CVarType::STRING;
		param.flags = flags;
		param.arrayIndex = static_cast<int32_t>(stringCVars.size());

		cvars[name] = param;
		CVarParameter* stored = &cvars[name];
		stringCVars.push_back({ defaultValue, currentValue, stored });
		return stored;
	}

	void DrawCVarEditor() override {
		static char filterText[128] = "";
		ImGui::InputText("Filter", filterText, sizeof(filterText));
		ImGui::Separator();

		for (auto& [name, param] : cvars) {
			if (param.HasFlag(CVarFlags::NoDisplay)) continue;
			if (filterText[0] != '\0' && name.find(filterText) == std::string::npos) continue;

			ImGui::PushID(name.c_str());
			switch (param.type) {
			case CVarType::INT:
			{
				if (param.HasFlag(CVarFlags::EditCheckBox)) {
					bool enabled = intCVars[param.arrayIndex].current != 0;
					if (ImGui::Checkbox(name.c_str(), &enabled)) {
						if (!param.HasFlag(CVarFlags::EditReadOnly)) {
							intCVars[param.arrayIndex].current = enabled ? 1 : 0;
						}
					}
				}
				else {
					int val = intCVars[param.arrayIndex].current;
					if (ImGui::InputInt(name.c_str(), &val)) {
						if (!param.HasFlag(CVarFlags::EditReadOnly)) {
							intCVars[param.arrayIndex].current = val;
						}
					}
				}
				break;
			}
			case CVarType::FLOAT:
			{
				float val = static_cast<float>(floatCVars[param.arrayIndex].current);
				if (param.HasFlag(CVarFlags::EditFloatSlider) || (param.min < param.max)) {
					float minVal = static_cast<float>(param.min);
					float maxVal = static_cast<float>(param.max);
					if (ImGui::SliderFloat(name.c_str(), &val, minVal, maxVal, "%.3f")) {
						if (!param.HasFlag(CVarFlags::EditReadOnly)) {
							floatCVars[param.arrayIndex].current = static_cast<double>(val);
						}
					}
				}
				else {
					if (ImGui::DragFloat(name.c_str(), &val, 0.05f)) {
						if (!param.HasFlag(CVarFlags::EditReadOnly)) {
							floatCVars[param.arrayIndex].current = static_cast<double>(val);
						}
					}
				}
				break;
			}
			case CVarType::STRING:
			{
				std::string& str = stringCVars[param.arrayIndex].current;
				std::array<char, 256> buf{};
				strncpy(buf.data(), str.c_str(), sizeof(buf) - 1);
				if (ImGui::InputText(name.c_str(), buf.data(), sizeof(buf))) {
					if (!(param.HasFlag(CVarFlags::EditReadOnly))) {
						str = buf.data();
					}
				}
				break;
			}

			}
			if (!param.description.empty() && ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", param.description.c_str());
			}
			ImGui::PopID();
		}
	}

	std::vector<CVarStorage<int32_t>> intCVars;
	std::vector<CVarStorage<double>> floatCVars;
	std::vector<CVarStorage<std::string>> stringCVars;

private:
	std::unordered_map<std::string, CVarParameter> cvars;

};

CVarSystem* CVarSystem::Get()
{
	return CVarSystemImpl::Get();
}

double AutoCVar<double>::Get() {
	return CVarSystemImpl::Get()->floatCVars[parameter->arrayIndex].current;
}

double* AutoCVar<double>::GetPtr() {
	return &CVarSystemImpl::Get()->floatCVars[parameter->arrayIndex].current;
}

void AutoCVar<double>::Set(double val) {
	CVarSystemImpl::Get()->floatCVars[parameter->arrayIndex].current = val;
}

AutoCVar_Int::AutoCVar_Int(const char* name, const char* description, int32_t defaultValue, CVarFlags flags) {
	parameter = CVarSystem::Get()->CreateIntCVar(name, description, defaultValue, defaultValue, flags);
}

int32_t AutoCVar<int32_t>::Get() {
	return CVarSystemImpl::Get()->intCVars[parameter->arrayIndex].current;
}

int32_t* AutoCVar<int32_t>::GetPtr() {
	return &CVarSystemImpl::Get()->intCVars[parameter->arrayIndex].current;
}

void AutoCVar<int32_t>::Set(int32_t val) {
	CVarSystemImpl::Get()->intCVars[parameter->arrayIndex].current = val;
}

AutoCVar_String::AutoCVar_String(const char* name, const char* description, const char* defaultValue, CVarFlags flags) {
	parameter = CVarSystem::Get()->CreateStringCVar(name, description, defaultValue, defaultValue, flags);
}

std::string AutoCVar<std::string>::Get() {
	return CVarSystemImpl::Get()->stringCVars[parameter->arrayIndex].current;
}

std::string* AutoCVar<std::string>::GetPtr() {
	return &CVarSystemImpl::Get()->stringCVars[parameter->arrayIndex].current;
}

void AutoCVar<std::string>::Set(std::string val) {
	CVarSystemImpl::Get()->stringCVars[parameter->arrayIndex].current = std::move(val);
}

AutoCVar_Float::AutoCVar_Float(const char* name, const char* description, double defaultValue, CVarFlags flags, double min, double max)
{
	parameter = CVarSystem::Get()->CreateFloatCVar(name, description, defaultValue, defaultValue, flags, min, max);
}

AutoCVar_Float::AutoCVar_Float(const char* name, const char* description, double defaultValue, double min, double max, CVarFlags flags)
{
	parameter = CVarSystem::Get()->CreateFloatCVar(name, description, defaultValue, defaultValue, flags | CVarFlags::EditFloatSlider, min, max);
}
