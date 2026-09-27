#pragma once

#include <string>
#include <vector>
#include <variant>

using Metric = std::variant<uint32_t, int32_t, float, std::string>;

struct PassMetric
{
	std::string name;
	Metric value;
	std::string unit = "";
};

struct PassStats {
	std::string name;
	float passTimeMs{ 0.0f };
	std::vector<PassMetric> metrics;

	void clear() {
		metrics.clear();
		passTimeMs = 0.0f;
	}

	void add_metric(std::string label, uint32_t val, std::string unit = "") { metrics.push_back({ std::move(label), val, std::move(unit) }); }
	void add_metric(std::string label, int32_t val, std::string unit = "") { metrics.push_back({ std::move(label), val, std::move(unit) }); }
	void add_metric(std::string label, float val, std::string unit = "") { metrics.push_back({ std::move(label), val, std::move(unit) }); }
	void add_metric(std::string label, std::string val) { metrics.push_back({ std::move(label), std::move(val), "" }); };
};

// base class for pass metrics 
class Pass {
public:
	virtual ~Pass() = default;
	virtual void cleanup() = 0;
	const PassStats& get_stats() const { return _stats; }

protected:
	PassStats _stats{};
};
