#include "Sampler.h"

#include <graphics/VulkanCore.h>
#include <fmt/core.h>

bool Sampler::doLoad()
{
	if (vkCreateSampler(_core->get_device(), &_info, nullptr, &_sampler) != VK_SUCCESS) {
		fmt::println("Sampler: failed to create sampler for: {}", get_id());
		return false;
	}
	return true;
}

bool Sampler::doUnload()
{
	if (_sampler != VK_NULL_HANDLE) {
		_core->get_deletion_queue().push(_sampler);
		_sampler = VK_NULL_HANDLE;
	}

	return true;
}
