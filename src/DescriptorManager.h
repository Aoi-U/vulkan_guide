#include <utils/vk_types.h>
#include <utils/vk_descriptors.h>

class DescriptorManager
{
public:
	void init(VkDevice device, VkImageView drawImageView);
	void cleanup(VkDevice device);

	// getters
	VkDescriptorSet get_draw_image_descriptor_set() const { return _drawImageDescriptors; }
	VkDescriptorSetLayout get_draw_image_descriptor_layout() const { return _drawImageDescriptorLayout; }

private:
	DescriptorAllocator _globalDescriptorAllocator;
	VkDescriptorSet _drawImageDescriptors;
	VkDescriptorSetLayout _drawImageDescriptorLayout;
};