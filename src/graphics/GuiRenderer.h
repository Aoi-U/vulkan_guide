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

private:
	VulkanCore* _vulkanCore;
	VkDescriptorPool _imguiDescriptorPool;
};