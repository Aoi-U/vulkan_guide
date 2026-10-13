#pragma once

#include <resources/ResourceManager.h>
#include <graphics/utils/vk_types.h>

class Sampler : public Resource {
public:
  Sampler(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, const VkSamplerCreateInfo& info) 
    : Resource(id, core, resourceManager), _info(info) {}
  ~Sampler() override { Resource::unload(); }

  VkSampler get_sampler() const { return _sampler; }

protected:
  bool doLoad() override;
  bool doUnload() override;

private:
  VkSamplerCreateInfo _info;
  VkSampler _sampler{ VK_NULL_HANDLE };
};