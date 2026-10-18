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
	VkDescriptorSet get_image_descriptor(VkImageView imageView, VkSampler sampler = VK_NULL_HANDLE, VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	// clear cached descriptors 
	void clear_image_descriptors();

private:
	VulkanCore* _vulkanCore;
	VkDescriptorPool _imguiDescriptorPool;

	VkFormat _colorFormat;

	std::unordered_map<VkImageView, VkDescriptorSet> _imageDescriptorCache;
};