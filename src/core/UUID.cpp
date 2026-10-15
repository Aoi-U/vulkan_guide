#include "UUID.h"

#include <random>
#include <array>

namespace
{
	// fixed namespace UUID for deterministic string to UUID generation
	constexpr std::array<uint8_t, 16> ENGINE_NAMESPACE_BYTES = {
		0x6b, 0xa7, 0xb8, 0x10, 0x9d, 0xad, 0x11, 0xd1,
		0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8
	};

	uuids::uuid get_engine_namespace_uuid()
	{
		return uuids::uuid(ENGINE_NAMESPACE_BYTES);
	}
}

UUID UUID::generate()
{
	// thread_local generator to avoid lock contention during async importing/streaming
	thread_local std::mt19937 generator([]() {
		std::random_device rd;
		auto seedData = std::array<int, std::mt19937::state_size>{};
		std::generate(std::begin(seedData), std::end(seedData), std::ref(rd));
		std::seed_seq seq(std::begin(seedData), std::end(seedData));

		return std::mt19937(seq);
		}());

	thread_local uuids::uuid_random_generator uuidGen{ generator };
	return UUID(uuidGen());
}

UUID UUID::derive(const UUID& parent, std::string_view subName)
{
	const uuids::uuid base = parent.is_valid() ? parent.raw() : get_engine_namespace_uuid();
	uuids::uuid_name_generator nameGen(base);

	return UUID(nameGen(subName));
}

UUID UUID::from_name(std::string_view name)
{
	if (name.empty()) 
		return UUID::null();
	uuids::uuid_name_generator nameGen(get_engine_namespace_uuid());

	return UUID(nameGen(name));
}

UUID UUID::from_string(std::string_view str)
{
	auto parsed = uuids::uuid::from_string(str);
	if (parsed.has_value()) {
		return UUID(parsed.value());
	}

	return UUID::null();
}

uint64_t UUID::to_u64() const noexcept
{ // hash UUID to 64 bit integer
	const auto bytes = _uuid.as_bytes();
	uint64_t low = 0, high = 0;
	memcpy(&low, bytes.data(), sizeof(uint64_t));
	memcpy(&high, bytes.data() + sizeof(uint64_t), sizeof(uint64_t));
	return low ^ (high + 0x9e3779b97f4a7c15ULL + (low << 6) + (low >> 2));
}
