#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>

struct ProfileMetric
{
	std::string name;
	float cpuTimeMs{ 0.0f };
	float gpuTimeMs{ 0.0f };
	float avgCpuTimeMs{ 0.0f };
	float avgGpuTimeMs{ 0.0f };
};

class Profiler
{
public:
	static constexpr uint32_t MAX_QUERIES = 256;

	static Profiler& get()
	{
		static Profiler instance;
		return instance;
	}

	void init(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t frameOverlap);
	void cleanup();

	void begin_frame(uint32_t frameIndex, VkCommandBuffer cmd);
	void end_frame();

	void push_cpu_scope(const std::string& name);
	void pop_cpu_scope(const std::string& name);

	void push_gpu_scope(const std::string& name, VkCommandBuffer cmd);
	void pop_gpu_scope(const std::string& name, VkCommandBuffer cmd);

	const std::vector<ProfileMetric>& get_metrics() const { return _metrics; }
	float get_total_cpu_time() const { return _totalCpuTimeMs; }
	float get_total_gpu_time() const { return _totalGpuTimeMs; }

	void draw_imgui();

private:
	Profiler() = default;
	~Profiler() = default;

	struct GpuQuery
	{
		std::string name;
		uint32_t startQueryIndex{ 0 };
		uint32_t endQueryIndex{ 0 };
	};

	struct FrameGpuData
	{
		VkQueryPool queryPool{ VK_NULL_HANDLE };
		uint32_t queryCount{ 0 };
		bool hasSubmitted{ false };
		std::vector<GpuQuery> queries;
	};

	VkDevice _device{ VK_NULL_HANDLE };
	float _timestampPeriod{ 1.0f }; // ns per tick
	uint64_t _validBitsMask{ UINT64_MAX };

	uint32_t _frameOverlap{ 2 };
	uint64_t _currentFrameIndex{ 0 };
	std::vector<FrameGpuData> _gpuFrames;

	struct CpuEntry
	{
		std::string name;
		std::chrono::steady_clock::time_point startTime;
	};

	std::vector<CpuEntry> _cpuStack;
	std::unordered_map<std::string, float> _currentFrameCpu;

	std::vector<ProfileMetric> _metrics;
	std::unordered_map<std::string, ProfileMetric> _metricHistory;
	float _totalCpuTimeMs{ 0.0 };
	float _totalGpuTimeMs{ 0.0 };
};

class ScopedCpuTimer
{
public:
	explicit ScopedCpuTimer(const std::string& name)
		: _name(name)
	{
		Profiler::get().push_cpu_scope(name);
	}

	~ScopedCpuTimer()
	{
		Profiler::get().pop_cpu_scope(_name);
	}

private:
	std::string _name;
};

class ScopedGpuTimer
{
public:
	ScopedGpuTimer(const std::string& name, VkCommandBuffer cmd)
		: _cmd(cmd), _name(name)
	{
		Profiler::get().push_gpu_scope(_name, _cmd);
	}

	~ScopedGpuTimer()
	{
		Profiler::get().pop_gpu_scope(_name, _cmd);
	}

private:
	std::string _name;
	VkCommandBuffer _cmd;
};

class ScopedCombinedTimer
{
public:
	ScopedCombinedTimer(const std::string& name, VkCommandBuffer cmd)
		: _name(name), _cmd(cmd)
	{
		Profiler::get().push_cpu_scope(_name);
		Profiler::get().push_gpu_scope(_name, _cmd);
	}

	~ScopedCombinedTimer()
	{
		Profiler::get().pop_gpu_scope(_name, _cmd);
		Profiler::get().pop_cpu_scope(_name);
	}
private:
	VkCommandBuffer _cmd;
	std::string _name;
};

#define PROFILE_CONCAT_INTERNAL(x, y) x##y
#define PROFILE_CONCAT(x, y) PROFILE_CONCAT_INTERNAL(x, y)

#define PROFILE_CPU(name) ScopedCpuTimer PROFILE_CONCAT(_cpuTimer_, __LINE__)(name)
#define PROFILE_GPU(name, cmd) ScopedGpuTimer PROFILE_CONCAT(_gpuTimer_, __LINE__)(name, cmd)
#define PROFILE_SCOPE(name, cmd) ScopedCombinedTimer PROFILE_CONCAT(_combTimer_, __LINE__)(name, cmd)
