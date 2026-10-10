#include "graphics/GuiRenderer.h"
#include <graphics/VulkanCore.h>
#include <utils/vk_initializers.h>
#include <utils/vk_utils.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_vulkan.h>
#include <ImGuizmo.h>
#include <array>


void GuiRenderer::init(Window* window, VulkanCore* vulkanCore, VkFormat colorFormat)
{
	_vulkanCore = vulkanCore;
	_colorFormat = colorFormat;

	// create descriptor pool for imgui
	// size of the pool is very oversize, but its copied from imgui demo 
	VkDescriptorPoolSize poolSizes[] = {
		{VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
		{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
	};

	VkDescriptorPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	poolInfo.maxSets = 1000;
	poolInfo.poolSizeCount = (uint32_t)std::size(poolSizes);
	poolInfo.pPoolSizes = poolSizes;

	VK_CHECK(vkCreateDescriptorPool(_vulkanCore->get_device(), &poolInfo, nullptr, &_imguiDescriptorPool));

	// initialize imgui library

	// initialize core imgui structures
	ImGui::CreateContext();

	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

	// initialize imgui for SDL
	ImGui_ImplSDL2_InitForVulkan(window->get_sdl_window());

	// initialize imgui for vulkan
	ImGui_ImplVulkan_InitInfo initInfo = {};
	initInfo.ApiVersion = VK_API_VERSION_1_3;
	initInfo.Instance = _vulkanCore->get_instance();
	initInfo.PhysicalDevice = _vulkanCore->get_physical_device();
	initInfo.Device = _vulkanCore->get_device();
	initInfo.QueueFamily = _vulkanCore->get_graphics_queue_family();
	initInfo.Queue = _vulkanCore->get_graphics_queue();
	initInfo.DescriptorPool = _imguiDescriptorPool;
	initInfo.MinImageCount = 3;
	initInfo.ImageCount = 3;
	initInfo.UseDynamicRendering = true;

	// configure main pipeline for dynamic rendering
	initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
	initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &_colorFormat;
	initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

	ImGui_ImplVulkan_Init(&initInfo);
}

void GuiRenderer::cleanup()
{
	ImGui_ImplVulkan_Shutdown();

	vkDestroyDescriptorPool(_vulkanCore->get_device(), _imguiDescriptorPool, nullptr);
}

void GuiRenderer::new_frame()
{
	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplSDL2_NewFrame();
	ImGui::NewFrame();
	ImGuizmo::BeginFrame();
}

void GuiRenderer::render(VkCommandBuffer cmd, VkImageView targetImageView, VkExtent2D drawExtent)
{
	// make imgui calculate internal draw structures
	ImGui::Render();

	VkClearValue clearColor = { { { 0.1f, 0.1f, 0.12f, 1.0f } } };
	VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(targetImageView, &clearColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingInfo renderInfo = vkinit::rendering_info(drawExtent, &colorAttachment, nullptr);

	vkCmdBeginRendering(cmd, &renderInfo);

	ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

	vkCmdEndRendering(cmd);
}

VkDescriptorSet GuiRenderer::get_image_descriptor(VkImageView imageView, VkSampler sampler, VkImageLayout layout)
{
	if (imageView == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;

	// check if we already have a descriptor set for this image view
	auto it = _imageDescriptorCache.find(imageView); 
	if (it != _imageDescriptorCache.end()) {
		return it->second;
	}

	// cache a new descriptor set for this image view
	VkDescriptorSet newSet = ImGui_ImplVulkan_AddTexture(sampler, imageView, layout);
	_imageDescriptorCache[imageView] = newSet;
	return newSet;
}

void GuiRenderer::clear_image_descriptors()
{
	for (auto& [imageView, descriptorSet] : _imageDescriptorCache) {
		if (descriptorSet != VK_NULL_HANDLE) {
			ImGui_ImplVulkan_RemoveTexture(descriptorSet);
		}
	}
	_imageDescriptorCache.clear();
}
