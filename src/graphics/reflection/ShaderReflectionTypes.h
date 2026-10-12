#pragma once
#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <span>

constexpr uint32_t BINDLESS_DESCRIPTOR_COUNT = 10000;

struct ReflectedVariable {
  std::string name;
  uint32_t offset{ 0 };
  uint32_t size{ 0 };
};

struct ReflectedBinding {
  std::string name;
  uint32_t set{ 0 };
  uint32_t binding{ 0 };
  uint32_t descriptorCount{ 1 };
  uint32_t blockByteSize{ 0 };
  VkDescriptorType descriptorType{ VK_DESCRIPTOR_TYPE_MAX_ENUM };
  VkShaderStageFlags stageFlags{ 0 };
  bool isVariableCount{ false };
  std::unordered_map<std::string, ReflectedVariable> variables;
};

struct ReflectedPushConstant {
  std::string name;
  uint32_t offset{ 0 };
  uint32_t size{ 0 };
  VkShaderStageFlags stageFlags{ 0 };
  std::unordered_map<std::string, ReflectedVariable> variables;
};

struct ReflectedShaderStage {
  VkShaderStageFlagBits stage;
  std::vector<ReflectedBinding> bindings;
  std::vector<ReflectedPushConstant> pushConstants;
};

struct PipelineLayoutReflection {
  std::unordered_map<uint32_t, std::vector<ReflectedBinding>> sets;
  std::vector<VkPushConstantRange> pushConstantRanges;
  std::unordered_map<std::string, ReflectedBinding> bindingsByName;
  std::unordered_map<std::string, ReflectedVariable> pushConstantsByName;
  std::unordered_map<std::string, ReflectedVariable> materialVariablesByName;
};
