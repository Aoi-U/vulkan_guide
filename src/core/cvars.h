#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <string_view>

enum class CVarFlags : uint32_t
{
	None = 0,
	NoDisplay = 1 << 0,
	EditReadOnly = 1 << 1,
	Advanced = 1 << 2,
	EditCheckBox = 1 << 3,
	EditFloatSlider = 1 << 4,
	EditInt = 1 << 5,
	EditIntSlider = 1 << 6
};

inline CVarFlags operator|(CVarFlags a, CVarFlags b)
{
	return static_cast<CVarFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline CVarFlags operator&(CVarFlags a, CVarFlags b)
{
	return static_cast<CVarFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

enum class CVarType : uint8_t
{
	INT,
	FLOAT,
	STRING
};

class CVarParameter
{
public:
	int32_t arrayIndex;

	CVarType type;
	CVarFlags flags;
	std::string name;
	std::string description;
	double min{ 0.0 };
	double max{ 1.0 };

	bool HasFlag(CVarFlags flag) const {
		return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
	}
};

class CVarSystem
{
public:
	static CVarSystem* Get();

	virtual CVarParameter* GetCVar(std::string_view name) = 0;

	virtual double GetFloatCVar(std::string_view name) = 0;
	virtual int32_t GetIntCVar(std::string_view name) = 0;
	virtual const char* GetStringCVar(std::string_view name) = 0;

	virtual void SetFloatCVar(std::string_view name, double value) = 0;
	virtual void SetIntCVar(std::string_view name, int32_t value) = 0;
	virtual void SetStringCVar(std::string_view name, const char* value) = 0;

	virtual CVarParameter* CreateFloatCVar(const char* name, const char* description, double defaultValue, double currentValue, CVarFlags flags = CVarFlags::None, double min = 0.0, double max = 1.0) = 0;
	virtual CVarParameter* CreateIntCVar(const char* name, const char* description, int32_t defaultValue, int32_t currentValue, CVarFlags flags = CVarFlags::None, int32_t min = 0, int32_t max = 0) = 0;
	virtual CVarParameter* CreateStringCVar(const char* name, const char* description, const char* defaultValue, const char* currentValue, CVarFlags flags = CVarFlags::None) = 0;

	virtual void DrawCVarEditor() = 0;
};

template<typename T>
struct AutoCVar
{
	CVarParameter* parameter;

	T Get();
	T* GetPtr();
	void Set(T val);
};

struct AutoCVar_Float : AutoCVar<double>
{
	AutoCVar_Float(const char* name, const char* description, double defaultValue, CVarFlags flags = CVarFlags::None, double min = 0.0, double max = 1.0);
	AutoCVar_Float(const char* name, const char* description, double defaultValue, double min, double max, CVarFlags flags = CVarFlags::EditFloatSlider);
};

struct AutoCVar_Int : AutoCVar<int32_t> {
	AutoCVar_Int(const char* name, const char* description, int32_t defaultValue, CVarFlags flags = CVarFlags::None, int32_t min = 0, int32_t max = 0);
	AutoCVar_Int(const char* name, const char* description, int32_t defaultValue, int32_t min, int32_t max, CVarFlags flags = CVarFlags::EditIntSlider);
};

struct AutoCVar_String : AutoCVar<std::string> {
	AutoCVar_String(const char* name, const char* description, const char* defaultValue, CVarFlags flags = CVarFlags::None);
};