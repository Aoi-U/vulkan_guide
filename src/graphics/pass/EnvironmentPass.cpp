#include "EnvironmentPass.h"

#include <graphics/VulkanCore.h>
#include <graphics/Renderer.h>
#include <graphics/utils/vk_utils.h>
#include <resources/DefaultResources.h>
#include <stb_image.h>
#include <cmath>

struct PrefilterPushConstants
{
  float roughness;
  float resolution;
};

void EnvironmentPass::init(VulkanCore* core, ResourceManager* rm)
{
  _core = core;
  _rm = rm;
  VkDevice device = _core->get_device();

  auto eqShader = rm->load<Shader>("equirect_to_cubemap.comp", VK_SHADER_STAGE_COMPUTE_BIT, "main");
  _equirectToCubemapPipeline = ComputePipeline(device, rm, { .computeShader = eqShader });

  auto irrShader = rm->load<Shader>("irradiance_cube.comp", VK_SHADER_STAGE_COMPUTE_BIT, "main");
  _irradiancePipeline = ComputePipeline(device, rm, { .computeShader = irrShader });

  auto prefShader = rm->load<Shader>("prefilter_cube.comp", VK_SHADER_STAGE_COMPUTE_BIT, "main");
  _prefilterPipeline = ComputePipeline(device, rm, { .computeShader = prefShader });

  auto brdfShader = rm->load<Shader>("brdf_lut.comp", VK_SHADER_STAGE_COMPUTE_BIT, "main");
  _brdfLutPipeline = ComputePipeline(device, rm, { .computeShader = brdfShader });

  // allocate persistent brdf lookuptable
  TextureDescription lutDesc{
    .extent = { 512, 512, 1 },
    .format = VK_FORMAT_R16G16_SFLOAT,
    .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
    .mipLevels = 1,
    .arrayLayers = 1,
    .viewType = VK_IMAGE_VIEW_TYPE_2D
  };
  _brdfLut = _rm->load<Texture>("brdfLut", lutDesc);

  request_procedural_sky();
}

void EnvironmentPass::cleanup()
{
  _hdrSourceTex = {};
  _envCubemap = {};
  _irradianceCubemap = {};
  _prefilterCubemap = {};
  _brdfLut = {};
  _equirectToCubemapPipeline.cleanup();
  _irradiancePipeline.cleanup();
  _prefilterPipeline.cleanup();
  _brdfLutPipeline.cleanup();
}

void EnvironmentPass::add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view)
{
  auto& sceneTargets = blackboard.get<SceneRenderContext>();

  // import persistent IBL textures into rendergraph
  VkImageLayout initialBrdfLayout =
    _brdfLutDirty ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkImageLayout initialEnvLayout = _environmentDirty
                                     ? VK_IMAGE_LAYOUT_UNDEFINED
                                     : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  sceneTargets.brdfLut = graph.import_texture("IBL_BrdfLut", _brdfLut, initialBrdfLayout);
  sceneTargets.envCubemap = graph.import_texture("IBL_EnvCubemap", _envCubemap,
                                                 _environmentDirty && !_useHdr
                                                   ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                   : initialEnvLayout);
  sceneTargets.irradianceMap = graph.import_texture("IBL_Irradiance", _irradianceCubemap, initialEnvLayout);
  sceneTargets.prefilterMap = graph.import_texture("IBL_Prefilter", _prefilterCubemap, initialEnvLayout);

  // add onetime brdf lut compute pass into rendergraph
  if (_brdfLutDirty)
  {
    struct BrdfPassData
    {
      RGTextureHandle lut;
    };
    graph.add_pass<BrdfPassData>("IBL_GenerateBrdfLUT",
                                 [&](RGPassBuilder& builder, BrdfPassData& data)
                                 {
                                   data.lut = builder.write_texture(sceneTargets.brdfLut,
                                                                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                                    VK_IMAGE_LAYOUT_GENERAL);
                                 },
                                 [this, &view](const BrdfPassData& data, RenderContext& rc)
                                 {
                                   VkDescriptorSet set = view.frameData._frameDescriptors.allocate(
                                     _core->get_device(), _brdfLutPipeline.get_descriptor_set_layout(0));
                                   DescriptorWriter writer;
                                   writer.write_image(0, _brdfLut->get_image_view(), VK_NULL_HANDLE,
                                                      VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
                                   writer.update_set(_core->get_device(), set);

                                   rc.bind_pipeline(_brdfLutPipeline);
                                   rc.bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, _brdfLutPipeline.get_layout(),
                                                          0, set);
                                   rc.dispatch(512 / 16, 512 / 16, 1);
                                 });
    _brdfLutDirty = false;
  }

  // add environment convolution pass into rendergraph
  if (_environmentDirty)
  {
    struct IBLBakeData
    {
      RGTextureHandle envCube;
      RGTextureHandle irradiance;
      RGTextureHandle prefilter;
    };

    graph.add_pass<IBLBakeData>("IBL_BakeEnvironment",
                                [&](RGPassBuilder& builder, IBLBakeData& data)
                                {
                                  if (_useHdr)
                                  {
                                    data.envCube = builder.write_texture(
                                      sceneTargets.envCubemap, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                      VK_IMAGE_LAYOUT_GENERAL);
                                  }
                                  else
                                  {
                                    data.envCube = builder.read_texture(
                                      sceneTargets.envCubemap, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                                  }
                                  data.irradiance = builder.write_texture(
                                    sceneTargets.irradianceMap, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                    VK_IMAGE_LAYOUT_GENERAL);
                                  data.prefilter = builder.write_texture(
                                    sceneTargets.prefilterMap, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                    VK_IMAGE_LAYOUT_GENERAL);
                                },
                                [this, &view, &graph, &sceneTargets](const IBLBakeData& data, RenderContext& rc)
                                {
                                  VkCommandBuffer cmd = rc.get_cmd();
                                  VkDevice dev = _core->get_device();
                                  auto& defaults = DefaultResources::get();
                                  uint32_t envSize = _envCubemap->get_extent().width;

                                  // create equirectangular to cubemap + mip chain if hdr
                                  if (_useHdr && _hdrSourceTex.is_valid())
                                  {
                                    VkDescriptorSet eqSet = view.frameData._frameDescriptors.allocate(
                                      dev, _equirectToCubemapPipeline.get_descriptor_set_layout(0));
                                    DescriptorWriter eqWriter;
                                    eqWriter.write_image(0, _envCubemap->get_mip_array_view(0), VK_NULL_HANDLE,
                                                         VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
                                    eqWriter.write_image(1, _hdrSourceTex->get_image_view(),
                                                         defaults.hdrSampler->get_sampler(),
                                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
                                    eqWriter.update_set(dev, eqSet);

                                    rc.bind_pipeline(_equirectToCubemapPipeline);
                                    rc.bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE,
                                                           _equirectToCubemapPipeline.get_layout(), 0, eqSet);
                                    rc.dispatch(envSize / 16, envSize / 16, 6);

                                    generate_cubemap_mips(cmd, _envCubemap->get_image(), envSize,
                                                          _envCubemap->get_mip_levels(), VK_IMAGE_LAYOUT_GENERAL,
                                                          _envCubemap->get_format());
                                    graph.set_texture_layout(data.envCube, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                                  }

                                  // create irradiance convolution pass (64x64)
                                  VkDescriptorSet irrSet = view.frameData._frameDescriptors.allocate(
                                    dev, _irradiancePipeline.get_descriptor_set_layout(0));
                                  DescriptorWriter irrWriter;
                                  irrWriter.write_image(0, _irradianceCubemap->get_mip_array_view(0), VK_NULL_HANDLE,
                                                        VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
                                  irrWriter.write_image(1, _envCubemap->get_image_view(),
                                                        defaults.cubemapSampler->get_sampler(),
                                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
                                  irrWriter.update_set(dev, irrSet);

                                  rc.bind_pipeline(_irradiancePipeline);
                                  rc.bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE,
                                                         _irradiancePipeline.get_layout(), 0, irrSet);
                                  rc.dispatch(64 / 16, 64 / 16, 6);

                                  // create specular prefilter convolution pass (128x128, 5 mips)
                                  rc.bind_pipeline(_prefilterPipeline);
                                  uint32_t mips = _prefilterCubemap->get_mip_levels();
                                  for (uint32_t mip = 0; mip < mips; ++mip)
                                  {
                                    uint32_t mipDim = std::max(1u, 128u >> mip);
                                    VkDescriptorSet prefSet = view.frameData._frameDescriptors.allocate(
                                      dev, _prefilterPipeline.get_descriptor_set_layout(0));
                                    DescriptorWriter prefWriter;
                                    prefWriter.write_image(0, _prefilterCubemap->get_mip_array_view(mip),
                                                           VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                                                           VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
                                    prefWriter.write_image(1, _envCubemap->get_image_view(),
                                                           defaults.cubemapSampler->get_sampler(),
                                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                           VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
                                    prefWriter.update_set(dev, prefSet);

                                    rc.bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE,
                                                           _prefilterPipeline.get_layout(), 0, prefSet);
                                    PrefilterPushConstants push{
                                      .roughness = static_cast<float>(mip) / static_cast<float>(mips - 1),
                                      .resolution = static_cast<float>(envSize)
                                    };
                                    rc.set_push_constants(_prefilterPipeline.get_layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                                                          push);
                                    rc.dispatch(std::max(1u, mipDim / 16), std::max(1u, mipDim / 16), 6);
                                  }
                                });
    _environmentDirty = false;
  }
}

bool EnvironmentPass::request_hdr_load(const std::filesystem::path& hdrPath, UUID assetId)
{
  int width, height, channels;
  float* data = stbi_loadf(hdrPath.string().c_str(), &width, &height, &channels, 4);
  if (!data)
  {
    fmt::println("EnvironmentPass: failed to load HDR image: {}", hdrPath.string());
    return false;
  }

  if (_rm) {
    _rm->force_unload<Texture>("ibl_envCubemap");
    _rm->force_unload<Texture>("ibl_irradianceCubemap");
    _rm->force_unload<Texture>("ibl_prefilterCubemap");
    _rm->force_unload<Texture>("ibl_hdr_source");
  }

  // release old textures
  _hdrSourceTex = {};
  _envCubemap = {};
  _irradianceCubemap = {};
  _prefilterCubemap = {};

  // create new hdr source texture
  size_t dataBytes = width * height * 4 * sizeof(float);
  TextureDescription hdrDesc{
    .extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 },
    .format = VK_FORMAT_R32G32B32A32_SFLOAT,
    .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
  };
  _hdrSourceTex = _rm->load<Texture>("ibl_hdr_source", hdrDesc,
                                     std::span<const uint8_t>(reinterpret_cast<uint8_t*>(data), dataBytes));
  stbi_image_free(data);

  // create new environment cubemap texture
  constexpr uint32_t ENV_SIZE = 2048;
  uint32_t envMips = static_cast<uint32_t>(std::floor(std::log2(ENV_SIZE))) + 1;
  TextureDescription envDesc{
    .extent = { ENV_SIZE, ENV_SIZE, 1 },
    .format = VK_FORMAT_R16G16B16A16_SFLOAT,
    .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    .mipLevels = envMips,
    .arrayLayers = 6,
    .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
    .viewType = VK_IMAGE_VIEW_TYPE_CUBE,
    .deferBindlessRegistration = true
  };
  _envCubemap = _rm->load<Texture>("ibl_envCubemap", envDesc);

  allocate_persistent_targets();

  _currentHdrPath = hdrPath;
  _currentHdrUUID = assetId.is_valid() ? assetId : AssetRegistry::get().find_uuid_by_path(hdrPath);

  _useHdr = true;
  _environmentDirty = true;
  return true;
}

void EnvironmentPass::request_procedural_sky()
{
  _hdrSourceTex = {};
  _envCubemap = {};
  _irradianceCubemap = {};
  _prefilterCubemap = {};

  upload_procedural_cubemap();
  allocate_persistent_targets();
  _currentHdrPath.clear();
  _currentHdrUUID = UUID::null();
  _useHdr = false;
  _environmentDirty = true;
}

void EnvironmentPass::allocate_persistent_targets()
{
  // create irradiance cubemap texture
  constexpr uint32_t IRR_SIZE = 64;
  TextureDescription irrDesc{
    .extent = { IRR_SIZE, IRR_SIZE, 1 },
    .format = VK_FORMAT_R16G16B16A16_SFLOAT,
    .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
    .mipLevels = 1,
    .arrayLayers = 6,
    .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
    .viewType = VK_IMAGE_VIEW_TYPE_CUBE,
    .deferBindlessRegistration = true
  };
  _irradianceCubemap = _rm->load<Texture>("ibl_irradianceCubemap", irrDesc);

  // create specular prefilter cubemap texture
  constexpr uint32_t PREF_SIZE = 128;
  constexpr uint32_t PREF_MIPS = 5;
  TextureDescription prefDesc{
    .extent = { PREF_SIZE, PREF_SIZE, 1 },
    .format = VK_FORMAT_R16G16B16A16_SFLOAT,
    .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
    .mipLevels = PREF_MIPS,
    .arrayLayers = 6,
    .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
    .viewType = VK_IMAGE_VIEW_TYPE_CUBE,
    .deferBindlessRegistration = true
  };
  _prefilterCubemap = _rm->load<Texture>("ibl_prefilterCubemap", prefDesc);
}

void EnvironmentPass::upload_procedural_cubemap()
{
  if (_rm) {
    _rm->force_unload<Texture>("ibl_envCubemap");
    _rm->force_unload<Texture>("ibl_irradianceCubemap");
    _rm->force_unload<Texture>("ibl_prefilterCubemap");
    _rm->force_unload<Texture>("ibl_hdr_source");
    _rm->force_unload<Buffer>("procedural_staging");
  }

  _envCubemap = {};
  _irradianceCubemap = {};
  _prefilterCubemap = {};

  // procedurally generated gradient cubemap 
  constexpr uint32_t FACE_SIZE = 256;
  const size_t faceBytes = FACE_SIZE * FACE_SIZE * 4 * sizeof(float);

  ResourceHandle<Buffer> staging = _rm->load<Buffer>("procedural_staging", faceBytes * 6,
                                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
  float* mappedData = static_cast<float*>(staging->get_mapped_data());

  // set colors for sky and 
  const glm::vec3 zenith(0.18f, 0.38f, 0.85f);
  const glm::vec3 horizon(0.72f, 0.82f, 0.95f);
  const glm::vec3 ground(0.22f, 0.22f, 0.25f);
  const glm::vec3 sunDir = glm::normalize(glm::vec3(0.4f, 1.0f, 0.3f));

  for (uint32_t face = 0; face < 6; ++face)
  {
    for (uint32_t y = 0; y < FACE_SIZE; ++y)
    {
      for (uint32_t x = 0; x < FACE_SIZE; ++x)
      {
        float u = (2.0f * (x + 0.5f) / FACE_SIZE) - 1.0f;
        float v = (2.0f * (y + 0.5f) / FACE_SIZE) - 1.0f;
        glm::vec3 dir(0.0f);
        switch (face)
        {
        case 0: dir = glm::vec3(1.0f, -v, -u);
          break; // +x
        case 1: dir = glm::vec3(-1.0f, -v, u);
          break; // -x
        case 2: dir = glm::vec3(u, 1.0f, v);
          break; // +y
        case 3: dir = glm::vec3(u, -1.0f, -v);
          break; // -y
        case 4: dir = glm::vec3(u, -v, 1.0f);
          break; // +z
        case 5: dir = glm::vec3(-u, -v, -1.0f);
          break; // -z
        }
        dir = glm::normalize(dir);
        glm::vec3 col = (dir.y > 0.0f) ? glm::mix(horizon, zenith, dir.y) : glm::mix(horizon, ground, -dir.y);
        float sunDot = glm::max(glm::dot(dir, sunDir), 0.0f);
        col += glm::vec3(1.0f, 0.95f, 0.8f) * std::pow(sunDot, 64.0f) * 1.5f;
        float iblIntensity = 0.05f;
        col *= iblIntensity;

        size_t offset = (face * FACE_SIZE * FACE_SIZE + y * FACE_SIZE + x) * 4;
        mappedData[offset + 0] = col.r;
        mappedData[offset + 1] = col.g;
        mappedData[offset + 2] = col.b;
        mappedData[offset + 3] = 1.0f;
      }
    }
  }

  // create cubemap image and upload data
  uint32_t envMips = static_cast<uint32_t>(std::floor(std::log2(FACE_SIZE))) + 1;

  TextureDescription envDesc{
    .extent = { FACE_SIZE, FACE_SIZE, 1 },
    .format = VK_FORMAT_R32G32B32A32_SFLOAT,
    .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    .mipLevels = envMips,
    .arrayLayers = 6,
    .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
    .viewType = VK_IMAGE_VIEW_TYPE_CUBE,
    .aspectMask = static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT),
    .deferBindlessRegistration = true
  };
  _envCubemap = _rm->load<Texture>("ibl_envCubemap", envDesc);

  // copy data from staging buffer to cubemap image and generate mips
  _core->immediate_submit([&](VkCommandBuffer cmd)
  {
    vkutil::image_mem_barrier2(cmd, _envCubemap->get_image(), _envCubemap->get_format(), VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, 1, 0);
    std::vector<VkBufferImageCopy> copies(6);
    for (uint32_t f = 0; f < 6; ++f)
    {
      copies[f].bufferOffset = f * faceBytes;
      copies[f].imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, f, 1 };
      copies[f].imageExtent = _envCubemap->get_extent();
    }
    vkCmdCopyBufferToImage(cmd, staging->get_buffer(), _envCubemap->get_image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           6, copies.data());
    generate_cubemap_mips(cmd, _envCubemap->get_image(), FACE_SIZE, envMips, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          _envCubemap->get_format());
  });
}

void EnvironmentPass::generate_cubemap_mips(VkCommandBuffer cmd, VkImage image, uint32_t size, uint32_t mipLevels,
                                            VkImageLayout currentLayout, VkFormat format)
{
  if (mipLevels <= 1)
  {
    vkutil::image_mem_barrier2(cmd, image, format, currentLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 6, 1, 0);
    return;
  }

  uint32_t mipWidth = size;
  uint32_t mipHeight = size;

  // transition the base mip level for blitting
  vkutil::image_mem_barrier2(cmd, image, format, currentLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 6, 1, 0);

  for (uint32_t mip = 0; mip < mipLevels - 1; mip++)
  {
    uint32_t nextWidth = std::max(1u, mipWidth / 2);
    uint32_t nextHeight = std::max(1u, mipHeight / 2);

    // transition next mip level to transfer dst
    vkutil::image_mem_barrier2(cmd, image, format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6,
                               1, mip + 1);

    // blit each face of the cubemap from current mip level to next mip level
    for (uint32_t face = 0; face < 6; face++)
    {
      VkImageBlit2 blitRegion{ .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2 };
      blitRegion.srcOffsets[1] = { static_cast<int32_t>(mipWidth), static_cast<int32_t>(mipHeight), 1 };
      blitRegion.dstOffsets[1] = { static_cast<int32_t>(nextWidth), static_cast<int32_t>(nextHeight), 1 };

      blitRegion.srcSubresource = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .mipLevel = mip,
        .baseArrayLayer = face,
        .layerCount = 1
      };

      blitRegion.dstSubresource = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .mipLevel = mip + 1,
        .baseArrayLayer = face,
        .layerCount = 1
      };

      VkBlitImageInfo2 blitInfo{
        .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
        .srcImage = image,
        .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .dstImage = image,
        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount = 1,
        .pRegions = &blitRegion,
        .filter = VK_FILTER_LINEAR
      };

      // blit image
      vkCmdBlitImage2(cmd, &blitInfo);
    }

    // transition current mip level to shaderreadonly
    vkutil::image_mem_barrier2(cmd, image, format, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 6, 1, mip);

    // prepare destination level to act as source for next step
    if (mip + 1 < mipLevels - 1)
    {
      vkutil::image_mem_barrier2(cmd, image, format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 6, 1, mip + 1);
    }
    else
    {
      // transition the final mip directly to shaderreadonly
      vkutil::image_mem_barrier2(cmd, image, format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 6, 1, mip + 1);
    }

    mipWidth = nextWidth;
    mipHeight = nextHeight;
  }
}
