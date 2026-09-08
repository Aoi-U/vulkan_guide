#include "DescriptorManager.h"

void DescriptorManager::init(VkDevice device, VkImageView drawImageView)
{
	// create a descriptor pool that will hold 10 sets with 1 image each
	std::vector<DescriptorAllocator::PoolSizeRatio> sizes =
	{
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}
	};

	_globalDescriptorAllocator.init_pool(device, 10, sizes);

	// make the descriptor set layout for our compute draw
	{
		DescriptorLayoutBuilder builder;
		builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		_drawImageDescriptorLayout = builder.build(device, VK_SHADER_STAGE_COMPUTE_BIT);
	}

	// allocate a descriptor set for our draw image
	_drawImageDescriptors = _globalDescriptorAllocator.allocate(device, _drawImageDescriptorLayout);

	DescriptorWriter writer;
	writer.write_image(0, drawImageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.update_set(device, _drawImageDescriptors);
}

void DescriptorManager::cleanup(VkDevice device)
{
	_globalDescriptorAllocator.destroy_pools(device);

	if (_drawImageDescriptorLayout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(device, _drawImageDescriptorLayout, nullptr);
	}
}
