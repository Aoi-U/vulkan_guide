#include "Profiler.h"

#include <imgui.h>
#include <fmt/core.h>
#include <algorithm>

void Profiler::init(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t frameOverlap)
{
	_device = device;
	_frameOverlap = frameOverlap;

	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(physicalDevice, &properties);
	_timestampPeriod = properties.limits.timestampPeriod;

	uint32_t queueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
	std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

	uint32_t validBits = queueFamilies[0].timestampValidBits;
	_validBitsMask = (validBits == 64) ? UINT64_MAX : ((1ull << validBits) - 1ull);

	// create a query pool for each frame 
	_gpuFrames.resize(frameOverlap);
	for (uint32_t i = 0; i < frameOverlap; i++) {
		VkQueryPoolCreateInfo poolInfo{
			.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
			.queryType = VK_QUERY_TYPE_TIMESTAMP,
			.queryCount = MAX_QUERIES
		};
		if (vkCreateQueryPool(_device, &poolInfo, nullptr, &_gpuFrames[i].queryPool) != VK_SUCCESS) {
			fmt::println("Profiler: failed to create timestamp query pool for frame {}", i);
		}
	}
}

void Profiler::cleanup()
{
	if (_device != VK_NULL_HANDLE) {
		for (auto& frame : _gpuFrames) {
			if (frame.queryPool != VK_NULL_HANDLE) {
				vkDestroyQueryPool(_device, frame.queryPool, nullptr);
				frame.queryPool = VK_NULL_HANDLE;
			}
		}
		_device = VK_NULL_HANDLE;
	}
	_gpuFrames.clear();
	_metricHistory.clear();
	_metrics.clear();

}

void Profiler::begin_frame(uint32_t frameIndex, VkCommandBuffer cmd)
{
	_currentFrameIndex = frameIndex;
	FrameGpuData& current = _gpuFrames[_currentFrameIndex];

	std::unordered_map<std::string, float> currentFrameGpu;

	if (current.hasSubmitted && current.queryCount > 0) { // get gpu results from previous frame
		std::vector<uint64_t> results(current.queryCount);
		VkResult res = vkGetQueryPoolResults(_device, current.queryPool, 0, current.queryCount, current.queryCount * sizeof(uint64_t), results.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);

		if (res == VK_SUCCESS) {
			for (const auto& query : current.queries) {
				uint64_t start = results[query.startQueryIndex] & _validBitsMask;
				uint64_t end = results[query.endQueryIndex] & _validBitsMask;

				uint64_t delta = (end >= start) ? (end - start) : ((end + (_validBitsMask + 1ull)) - start); // handle wrap around
				float durationMs = static_cast<float>(delta) * _timestampPeriod * 1e-6f; // convert to milliseconds
				currentFrameGpu[query.name] = durationMs;
			}
		}
	}

	_metrics.clear();
	_totalCpuTimeMs = 0.0f;
	_totalGpuTimeMs = 0.0f;

	// collect names from cpu and gpu results
	std::vector<std::string> names;
	for (const auto& [name, _] : _currentFrameCpu) 
		names.push_back(name);
	for (const auto& [name, _] : currentFrameGpu) {
		if (std::find(names.begin(), names.end(), name) == names.end()) {
			names.push_back(name);
		}
	}

	for (const auto& name : names) { // combine cpu and gpu results into metrics
		float cpu = _currentFrameCpu.contains(name) ? _currentFrameCpu[name] : 0.0f;
		float gpu = currentFrameGpu.contains(name) ? currentFrameGpu[name] : 0.0f;

		auto& history = _metricHistory[name];
		history.name = name;
		history.cpuTimeMs = cpu;
		history.gpuTimeMs = gpu;

		constexpr float alpha = 0.05f; // exponential moving average for smooth display
		history.avgCpuTimeMs = (history.avgCpuTimeMs == 0.0f) ? cpu : (history.avgCpuTimeMs * (1.0f - alpha) + cpu * alpha); 
		history.avgGpuTimeMs = (history.avgGpuTimeMs == 0.0f) ? gpu : (history.avgGpuTimeMs * (1.0f - alpha) + gpu * alpha);

		_metrics.push_back(history);
		_totalCpuTimeMs += cpu;
		_totalGpuTimeMs += gpu;
	}

	// reset gpu query pool for next use 
	vkCmdResetQueryPool(cmd, current.queryPool, 0, MAX_QUERIES);
	current.queryCount = 0;
	current.queries.clear();
	current.hasSubmitted = true;
	_currentFrameCpu.clear();
	_cpuStack.clear();
}

void Profiler::end_frame()
{}

void Profiler::push_cpu_scope(const std::string & name)
{
	_cpuStack.push_back({ name, std::chrono::steady_clock::now() });
}

void Profiler::pop_cpu_scope(const std::string & name)
{
	auto now = std::chrono::steady_clock::now();
	if (!_cpuStack.empty()) {
		auto top = _cpuStack.back();
		_cpuStack.pop_back();
		float ms = std::chrono::duration<float, std::milli>(now - top.startTime).count();
		_currentFrameCpu[top.name] += ms;
	}
}

void Profiler::push_gpu_scope(const std::string & name, VkCommandBuffer cmd)
{
	FrameGpuData& current = _gpuFrames[_currentFrameIndex];
	if (current.queryCount + 2 > MAX_QUERIES) {
		fmt::println("Profiler: exceeded max gpu queries for frame {}", _currentFrameIndex);
		return;
	}

	uint32_t startIndex = current.queryCount++;
	vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, current.queryPool, startIndex);
	current.queries.push_back(GpuQuery{
		.name = name,
		.startQueryIndex = startIndex,
		.endQueryIndex = 0
		});
}

void Profiler::pop_gpu_scope(const std::string & name, VkCommandBuffer cmd)
{
	FrameGpuData& current = _gpuFrames[_currentFrameIndex];
	if (current.queryCount >= MAX_QUERIES)
		return;

	for (auto it = current.queries.rbegin(); it != current.queries.rend(); it++) { // find and close most recent query with same name
		if (it->name == name && it->endQueryIndex == 0) {
			uint32_t endIndex = current.queryCount++;
			it->endQueryIndex = endIndex;
			vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, current.queryPool, endIndex);
			break;
		}
	}
}

void Profiler::draw_imgui()
{
	ImGui::Text("Total CPU: %.2f ms | Total GPU: %.2f ms", _totalCpuTimeMs, _totalGpuTimeMs);
	ImGui::Separator();

	static ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
		ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable;

	if (ImGui::BeginTable("ProfilingTimers", 5, flags)) {
		ImGui::TableSetupColumn("Scope / Pass", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("CPU (ms)", ImGuiTableColumnFlags_WidthFixed, 75.0f);
		ImGui::TableSetupColumn("CPU Avg", ImGuiTableColumnFlags_WidthFixed, 75.0f);
		ImGui::TableSetupColumn("GPU (ms)", ImGuiTableColumnFlags_WidthFixed, 75.0f);
		ImGui::TableSetupColumn("GPU Avg", ImGuiTableColumnFlags_WidthFixed, 75.0f);
		ImGui::TableHeadersRow();

		for (const auto& metric : _metrics) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(metric.name.c_str());

			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%.3f", metric.cpuTimeMs);

			ImGui::TableSetColumnIndex(2);
			ImGui::Text("%.3f", metric.avgCpuTimeMs);

			ImGui::TableSetColumnIndex(3);
			if (metric.gpuTimeMs > 0.0f) {
				ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "%.3f", metric.gpuTimeMs);
			}
			else {
				ImGui::TextDisabled("-");
			}

			ImGui::TableSetColumnIndex(4);
			if (metric.avgGpuTimeMs > 0.0f) {
				ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "%.3f", metric.avgGpuTimeMs);
			}
			else {
				ImGui::TextDisabled("-");
			}
		}
		ImGui::EndTable();
	}
}
