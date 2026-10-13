#include "ShaderReflection.h"

#include <spirv_reflect.h>
#include <fmt/core.h>
#include <algorithm>

namespace
{
	// recursively reflect block members 
	void reflect_block_variables(const SpvReflectBlockVariable& block, std::unordered_map<std::string, ReflectedVariable>& outVariables, const std::string& prefix = "")
	{
		for (uint32_t i = 0; i < block.member_count; i++) {
			const auto& m = block.members[i];
			std::string memberName = (m.name && m.name[0] != '\0') ? m.name : fmt::format("member_{}", i);
			std::string path = prefix.empty() ? memberName : (prefix + "." + memberName);

			outVariables[path] = ReflectedVariable{
				.name = path,
				.offset = m.offset,
				.size = m.size
			};

			if (m.member_count > 0) {
				reflect_block_variables(m, outVariables, path);
			}
			else {
				outVariables[path] = ReflectedVariable{
					.name = path,
					.offset = m.offset,
					.size = m.size
				};
			}
		}
	}
}


bool ShaderReflection::reflect_stage(std::span<const uint32_t> spirv, VkShaderStageFlagBits stage, ReflectedShaderStage& outStage)
{
	if (spirv.empty()) {
		fmt::println("ShaderReflection: spirv bytecode span is empty for stage {}", static_cast<uint32_t>(stage));
		return false;
	}

	SpvReflectShaderModule module;
	SpvReflectResult result = spvReflectCreateShaderModule(spirv.size() * sizeof(uint32_t), spirv.data(), &module);
	if (result != SPV_REFLECT_RESULT_SUCCESS) {
		fmt::println("ShaderReflection: failed to create SpvReflectShaderModule code {} for stage {}", static_cast<int>(result), static_cast<uint32_t>(stage));
		return false;
	}

	outStage.stage = stage;
	outStage.bindings.clear();
	outStage.pushConstants.clear();

	// reflect descriptor bindings
	uint32_t bindingCount = 0;
	spvReflectEnumerateDescriptorBindings(&module, &bindingCount, nullptr);
	if (bindingCount > 0) {
		std::vector<SpvReflectDescriptorBinding*> bindings(bindingCount);
		spvReflectEnumerateDescriptorBindings(&module, &bindingCount, bindings.data());

		for (const auto* b : bindings) {
			ReflectedBinding binding;
			binding.set = b->set;
			binding.binding = b->binding;
			binding.descriptorType = static_cast<VkDescriptorType>(b->descriptor_type);
			binding.descriptorCount = (b->count == 0) ? 1 : b->count;
			binding.stageFlags = stage;

			if (b->name && b->name[0] != '\0') {
				binding.name = b->name;
			}
			else if (b->type_description && b->type_description->type_name && b->type_description->type_name[0] != '\0') {
				binding.name = b->type_description->type_name;
			}

			binding.blockByteSize = b->block.size;

			// get runtime sized or bindless descriptor arrays
			if ((b->type_description && b->type_description->traits.array.dims_count > 0 && b->count == 0) || b->set == 1) {
				binding.isVariableCount = true;
				binding.descriptorCount = BINDLESS_DESCRIPTOR_COUNT;
			}

			// reflect uniform and storage buffer 
			if (b->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
				reflect_block_variables(b->block, binding.variables);
			}

			outStage.bindings.push_back(std::move(binding));
		}
	}

	// reflect push constants
	uint32_t pushConstantCount = 0;
	spvReflectEnumeratePushConstantBlocks(&module, &pushConstantCount, nullptr);
	if (pushConstantCount > 0) {
		std::vector<SpvReflectBlockVariable*> pushConstantBlocks(pushConstantCount);
		spvReflectEnumeratePushConstantBlocks(&module, &pushConstantCount, pushConstantBlocks.data());

		for (const auto* pc : pushConstantBlocks) {
			ReflectedPushConstant pushConstant{};
			pushConstant.stageFlags = stage;
			pushConstant.offset = pc->offset;
			pushConstant.size = pc->size;
			pushConstant.name = (pc->name && pc->name[0] != '\0') ? pc->name : "PushConstants";
			
			reflect_block_variables(*pc, pushConstant.variables);
			outStage.pushConstants.push_back(std::move(pushConstant));
		}
	}

	spvReflectDestroyShaderModule(&module);
	return true;
}

PipelineLayoutReflection ShaderReflection::merge_stages(std::span<const ReflectedShaderStage> stages)
{
	PipelineLayoutReflection reflection;

	for (const auto& stage : stages) {
		// merge descriptor bindings
		for (const auto& b : stage.bindings) {
			auto& setVec = reflection.sets[b.set];
			auto it = std::find_if(setVec.begin(), setVec.end(), [&](const ReflectedBinding& existing) { // find existing binding
				return existing.binding == b.binding;
				});

			if (it != setVec.end()) {  // merge with existing binding
				it->stageFlags |= b.stageFlags;
				if (b.isVariableCount) {
					it->isVariableCount = true;
					it->descriptorCount = std::max(it->descriptorCount, b.descriptorCount);
				}
			}
			else { // add new binding
				setVec.push_back(b);
			}

			if (!b.name.empty()) { // add binding name
				reflection.bindingsByName[b.name] = b;
			}

			for (const auto& [varName, var] : b.variables) { // add material variable names
				reflection.materialVariablesByName[varName] = var;
			}
		}

		// merge push constants
		for (const auto& pc : stage.pushConstants) {
			auto it = std::find_if(reflection.pushConstantRanges.begin(), reflection.pushConstantRanges.end(), [&](const VkPushConstantRange& range) {
				// check if ranges overlap or are identical
				uint32_t rStart = range.offset;
				uint32_t rEnd = range.offset + range.size;
				uint32_t pcStart = pc.offset;
				uint32_t pcEnd = pc.offset + pc.size;

				return std::max(rStart, pcStart) < std::min(rEnd, pcEnd) || (rStart == pcStart && rEnd == pcEnd); // check for overlap or identical
				});

			if (it != reflection.pushConstantRanges.end()) { // merge with existing range
				uint32_t newStart = std::min(it->offset, pc.offset);
				uint32_t newEnd = std::max(it->offset + it->size, pc.offset + pc.size);

				it->offset = newStart;
				it->size = newEnd - newStart;
				it->stageFlags |= pc.stageFlags;
			}
			else { // add new range
				reflection.pushConstantRanges.push_back(VkPushConstantRange{
					.stageFlags = pc.stageFlags,
					.offset = pc.offset,
					.size = pc.size
				});
			}

			for (const auto& [varName, var] : pc.variables) { // add push constant variable names
				reflection.pushConstantsByName[varName] = var;
			}
		}
	}

	// sort bindings in each set by binding number
	for (auto& [setIndex, bindings] : reflection.sets) {
		std::sort(bindings.begin(), bindings.end(), [](const ReflectedBinding& a, const ReflectedBinding& b){
			return a.binding < b.binding;
			});
	}


	// print names
	fmt::println("Names:\n");
	for (const auto& [name, var] : reflection.bindingsByName) {
		fmt::println("Binding: {} set {} binding {} type {} count {}", name, var.set, var.binding, static_cast<uint32_t>(var.descriptorType), var.descriptorCount);
	}
	
	for (const auto& [name, var] : reflection.materialVariablesByName) {
		fmt::println("Material Variable: {} offset {} size {}", name, var.offset, var.size);
	}
	for (const auto& [name, var] : reflection.pushConstantsByName) {
		fmt::println("Push Constant: {} offset {} size {}", name, var.offset, var.size);
	}

	return reflection;
}
