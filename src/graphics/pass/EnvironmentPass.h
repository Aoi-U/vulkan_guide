#pragma once

#include <graphics/pass/RenderFeature.h>
#include <graphics/Pipeline.h>
#include <resources/Texture.h>
#include <filesystem>
#include <cstdint>

class EnvironmentPass : public RenderFeature
{
public:
  EnvironmentPass()
    : RenderFeature("EnvironmentIBL", true)
  {
  }

  void init(VulkanCore* core, ResourceManager* rm) override;
  void cleanup() override;

  void add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view) override;

  bool request_hdr_load(const std::filesystem::path& hdrPath, UUID assetId = UUID::null());
  void request_procedural_sky();

  bool is_using_hdr() const noexcept { return _useHdr; }
  const std::filesystem::path& get_hdr_path() const noexcept { return _currentHdrPath; }
  UUID get_hdr_uuid() const noexcept { return _currentHdrUUID; }

private:
  void allocate_persistent_targets();
  void upload_procedural_cubemap();
  static void generate_cubemap_mips(VkCommandBuffer cmd, VkImage image, uint32_t size, uint32_t mipLevels,
                                    VkImageLayout currentLayout, VkFormat format);

  VulkanCore* _core{ nullptr };
  ResourceManager* _rm{ nullptr };

  // persistent targets
  ResourceHandle<Texture> _hdrSourceTex;
  ResourceHandle<Texture> _envCubemap;
  ResourceHandle<Texture> _irradianceCubemap;
  ResourceHandle<Texture> _prefilterCubemap;
  ResourceHandle<Texture> _brdfLut;

  // compute pipelines
  ComputePipeline _equirectToCubemapPipeline;
  ComputePipeline _irradiancePipeline;
  ComputePipeline _prefilterPipeline;
  ComputePipeline _brdfLutPipeline;

  bool _brdfLutDirty{ true };
  bool _environmentDirty{ true };
  bool _useHdr{ false };
  std::filesystem::path _currentHdrPath;
  UUID _currentHdrUUID{};
};
