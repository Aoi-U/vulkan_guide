#include "DefaultResources.h"

#include <glm/gtx/transform.hpp>
#include <array>

void DefaultResources::init(ResourceManager* manager)
{
	_manager = manager;

	// default textures
	uint32_t white = glm::packUnorm4x8(glm::vec4(1, 1, 1, 1));
	uint32_t grey = glm::packUnorm4x8(glm::vec4(0.66f, 0.66f, 0.66f, 1.f));
	uint32_t black = glm::packUnorm4x8(glm::vec4(0, 0, 0, 1));
	uint32_t normal = glm::packUnorm4x8(glm::vec4(0.5f, 0.5f, 1.0f, 1.0f));

	TextureDescription desc1{
		.extent = {1, 1, 1}, .format = VK_FORMAT_R8G8B8A8_UNORM,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
	};

  whiteImage = _manager->load<Texture>("default_white", desc1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&white), sizeof(uint32_t)));
  greyImage = _manager->load<Texture>("default_grey", desc1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&grey), sizeof(uint32_t)));
  blackImage = _manager->load<Texture>("default_black", desc1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&black), sizeof(uint32_t)));
  defaultNormalImage = _manager->load<Texture>("default_normal", desc1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&normal), sizeof(uint32_t)));

  // checkerboard
  uint32_t magenta = glm::packUnorm4x8(glm::vec4(1, 0, 1, 1));
  std::array<uint32_t, 16 * 16> pixels;
  for (int x = 0; x < 16; x++) {
    for (int y = 0; y < 16; y++) {
      pixels[y * 16 + x] = ((x % 2) ^ (y % 2)) ? magenta : black;
    }
  }

  TextureDescription desc2{
      .extent = {16, 16, 1}, 
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .magFilter = VK_FILTER_NEAREST,
      .minFilter = VK_FILTER_NEAREST
  };
  errorCheckerboardImage = _manager->load<Texture>("default_checkerboard", desc2, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(pixels.data()), sizeof(pixels)));

  // samplers
  VkSamplerCreateInfo nearestInfo = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST };
  defaultSamplerNearest = _manager->load<Sampler>("sampler_nearest", nearestInfo);

  VkSamplerCreateInfo linearInfo = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR };
  defaultSamplerLinear = _manager->load<Sampler>("sampler_linear", linearInfo);

  VkSamplerCreateInfo shadowInfo = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE };
  shadowSampler = _manager->load<Sampler>("sampler_shadow", shadowInfo);

  // default PBR material template
  auto gbufferVert = _manager->load<Shader>("gbuffer.vert", VK_SHADER_STAGE_VERTEX_BIT, "main");
  auto gbufferFrag = _manager->load<Shader>("gbuffer.frag", VK_SHADER_STAGE_FRAGMENT_BIT, "main");
  defaultPBRTemplate = _manager->load<MaterialTemplate>("pbr_default_template", gbufferVert, gbufferFrag);
}

void DefaultResources::cleanup()
{
  whiteImage = {};
  blackImage = {};
  greyImage = {};
  errorCheckerboardImage = {};
  defaultNormalImage = {};
  defaultSamplerLinear = {};
  defaultSamplerNearest = {};
  shadowSampler = {};
  defaultPBRTemplate = {};
}
