#pragma once

#include <stduuid/uuid.h>
#include <fmt/core.h>
#include <fmt/format.h>
#include <yaml-cpp/yaml.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <compare>
#include <functional>

class UUID
{
public:
	constexpr UUID() noexcept = default;

	explicit constexpr UUID(const uuids::uuid& id) noexcept
		: _uuid(id) {}

	static UUID generate();

	static UUID derive(const UUID& parent, std::string_view subName);

	static UUID from_name(std::string_view name);

	static UUID from_string(std::string_view str);

	static constexpr UUID null() noexcept { return UUID{}; }

	bool is_valid() const noexcept { return !_uuid.is_nil(); }

	explicit operator bool() const noexcept { return !_uuid.is_nil(); }

	std::string to_string() const { return uuids::to_string(_uuid); }

	uint64_t to_u64() const noexcept;

	const uuids::uuid& raw() const noexcept { return _uuid; }

	friend bool operator==(const UUID& a, const UUID& b) noexcept { return a._uuid == b._uuid; }
	friend bool operator!=(const UUID& a, const UUID& b) noexcept { return a._uuid != b._uuid; }
	friend bool operator<(const UUID& a, const UUID& b) noexcept { return a._uuid < b._uuid; }

private:
	uuids::uuid _uuid{};
};

namespace std
{
	template<>
	struct hash<UUID>
	{
		size_t operator()(const UUID& uuid) const noexcept
		{
			return static_cast<size_t>(uuid.to_u64());
		}
	};
}

template<>
struct fmt::formatter<UUID> : fmt::formatter<std::string_view>
{
	template<typename FormatContext>
	auto format(const UUID& uuid, FormatContext& ctx) const -> decltype(ctx.out())
	{
		return fmt::formatter<std::string_view>::format(uuid.to_string(), ctx);
	}
};

namespace YAML
{
	template<>
	struct convert<UUID>
	{
		static Node encode(const UUID& uuid)
		{
			return Node(uuid.to_string());
		}
		
		static bool decode(const Node& node, UUID& uuid)
		{
			if (!node.IsScalar())
				return false;

			std::string str = node.as<std::string>();
			if (str.empty() || str == "0" || str == "null") {
				uuid = UUID::null();
				return true;
			}
			uuid = UUID::from_string(str);
			return true;
		}
	};

	inline Emitter& operator<<(Emitter& out, const UUID& uuid)
	{
		return out << uuid.to_string();
	}
}