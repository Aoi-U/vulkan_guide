#include <utils/vk_utils.h>
#include <utils/vk_initializers.h>
#include <window/window.h>

class VulkanCore; 

class GuiRenderer
{
public:
	void init(Window* window, VulkanCore* vulkanCore, VkFormat colorFormat);
	void cleanup();

	void new_frame();
	void render(VkCommandBuffer cmd, VkImageView targetImageView, VkExtent2D drawExtent);

	// create/reuse a descriptor set for displaying an image in imgui
	VkDescriptorSet get_image_descriptor(VkImageView imageView, VkSampler sampler, VkImageLayout layout);

private:
	VulkanCore* _vulkanCore;
	VkDescriptorPool _imguiDescriptorPool;

	VkDescriptorSet _viewportDescriptorSet;
	VkImageView _viewportImageView;
	VkFormat _colorFormat;
};