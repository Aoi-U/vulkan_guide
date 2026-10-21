#pragma once
#include <graphics/utils/vk_types.h>
#include <graphics/VulkanCore.h>
#include <graphics/Swapchain.h>
#include <graphics/RenderGraph.h>
#include <graphics/utils/RGParameters.h>
#include <graphics/utils/RenderTargetPool.h>
#include <graphics/RGBlackboard.h>
#include <graphics/pass/RenderFeature.h>
#include <graphics/pass/EnvironmentPass.h>
#include <resources/Buffer.h>
#include <window/window.h>
#include <utils/vk_descriptors.h>
#include <memory>
#include <entt.hpp>
#include <vector>
#include <string_view>
#include <core/Events.h>
#include <core/cvars.h>

extern AutoCVar_Float cvRenderScale;
extern AutoCVar_Int cvBloom;
extern AutoCVar_Int cvSSAO;
extern AutoCVar_Float cvSSAORadius;
extern AutoCVar_Float cvSSAOBias;
extern AutoCVar_Float cvSSAOPower;

class GuiRenderer;
class ResourceManager;

struct IndirectBatch
{
  uint32_t first; // first index in the indirect buffer
  uint32_t count; // number of indirect commands in this batch
  uint32_t batchIndex; // index of this batch in the batch array
  bool isDoubleSided{ false };
};

struct DrawCommandCache
{
  std::vector<RenderObject> opaqueObjects;
  std::vector<RenderObject> transparentObjects;

  // indirect draw
  std::vector<IndirectBatch> opaqueBatches;
  bool isDirty{ true };
};

struct DrawContext
{
  std::span<const RenderObject> opaqueObjects;
  std::span<const RenderObject> transparentObjects;
  std::span<const IndirectBatch> opaqueBatches;

  // indirect draw 
  uint32_t indirectDrawCount{ 0 };
  VkDescriptorSet bindlessMaterialSet{ VK_NULL_HANDLE };

  void clear()
  {
    opaqueObjects = {};
    transparentObjects = {};
    opaqueBatches = {};
    indirectDrawCount = 0;
    bindlessMaterialSet = VK_NULL_HANDLE;
  }
};

struct FrameData
{
  VkCommandPool _commandPool;
  VkCommandBuffer _mainCommandBuffer;
  VkSemaphore _swapchainSemaphore;
  VkFence _renderFence;
  DescriptorAllocatorGrowable _frameDescriptors;

  ResourceHandle<Buffer> sceneDataBuffer; // uniform buffer for scene data
  ResourceHandle<Buffer> _cullStatsBuffer; // buffer for frustum cull statistics
  ResourceHandle<Buffer> _gpuObjectBuffer; // buffer for object data used in frustum culling
  ResourceHandle<Buffer> _gpuInstanceBuffer; // buffer for instance ids for visible objects
  ResourceHandle<Buffer> _gpuIndirectBuffer; // buffer for indirect draw commands
  ResourceHandle<Buffer> _gpuCountBuffer; // buffer for draw counts per batch
  ResourceHandle<Buffer> _materialDataBuffer; // buffer for material data for each entity in the scene
  ResourceHandle<Buffer> _gpuShadowIndirectBuffer; // buffer for indirect draw commands for shadow pass
  ResourceHandle<Buffer> _gpuShadowInstanceBuffer; // buffer for instance ids for visible objects in shadow pass
};

struct TargetInfo
{
  VkFormat format;
  VkExtent2D extent;
  VkImageView view;
  std::vector<VkImageView> faceViews;
  bool isCube;
};

class Renderer
{
public:
  static constexpr unsigned int FRAME_OVERLAP = 2;

  using OverlayRenderCallback = std::function<void(
    VkCommandBuffer cmd,
    VkImageView swapchainView,
    VkExtent2D swapchainExtent,
    VkImageView finalSceneColorView)>;

  using SwapchainRecreateCallback = std::function<void()>;

  void init(Window* window, VulkanCore* vulkanCore, entt::dispatcher& dispatcher, ResourceManager* resourceManager);

  void cleanup(entt::dispatcher& dispatcher);

  void on_window_resize(const Events::WindowResizeEvent& event);

  void set_overlay_render_callback(OverlayRenderCallback callback) { _overlayRenderCallback = std::move(callback); }

  void set_swapchain_recreate_callback(SwapchainRecreateCallback callback)
  {
    _swapchainRecreateCallback = std::move(callback);
  }

  void wait_for_current_frame_fence();

  void set_viewport_extent(VkExtent2D extent);
  void render_frame(const entt::registry& registry, const GPUSceneData& sceneData);
  void recreate_swapchain();

  void mark_scene_dirty() { _drawCache.isDirty = true; }
  bool load_environment(const std::filesystem::path& path, UUID assetId = UUID::null());
  void remove_environment();

  template <typename T, typename... Args>
  T* add_feature(Args&&... args)
  {
    static_assert(std::is_base_of_v<RenderFeature, T>, "T must derive from RenderFeature");
    auto feature = std::make_unique<T>(std::forward<Args>(args)...);
    feature->init(_vulkanCore, _resourceManager);
    T* ptr = feature.get();
    _features.push_back(std::move(feature));
    return ptr;
  }

  template <typename T, typename... Args>
  T* insert_feature_before(std::string_view targetPassName, Args&&... args)
  {
    static_assert(std::is_base_of_v<RenderFeature, T>, "T must derive from RenderFeature");
    auto feature = std::make_unique<T>(std::forward<Args>(args)...);
    feature->init(_vulkanCore, _resourceManager);
    T* ptr = feature.get();

    auto it = std::find_if(_features.begin(), _features.end(), [&](const auto& f)
    {
      return f->get_name() == targetPassName;
    });
    _features.insert(it, std::move(feature));
    return ptr;
  }

  bool remove_feature(std::string_view name)
  {
    auto it = std::find_if(_features.begin(), _features.end(), [&](const auto& f)
    {
      return f->get_name() == name;
    });
    if (it != _features.end())
    {
      vkDeviceWaitIdle(_vulkanCore->get_device());
      (*it)->cleanup();
      _features.erase(it);
      return true;
    }
    return false;
  }

  void remove_feature_at(size_t index)
  {
    if (index < _features.size())
    {
      vkDeviceWaitIdle(_vulkanCore->get_device());
      _features[index]->cleanup();
      _features.erase(_features.begin() + static_cast<std::ptrdiff_t>(index));
    }
  }

  RenderFeature* get_feature(std::string_view name) const;
  std::vector<std::unique_ptr<RenderFeature>>& get_features() { return _features; }
  const std::vector<std::unique_ptr<RenderFeature>>& get_features() const { return _features; }

  uint32_t get_frame_number() const { return _frameNumber; }
  VkExtent2D get_swapchain_extent() const { return _swapchain.get_extent(); }
  VkExtent2D get_draw_extent() const { return _drawExtent; }
  VkImageView get_current_swapchain_image_view() const { return _swapchain.get_image_views()[_swapchainImageIndex]; }
  VkFormat get_swapchain_format() const { return _swapchain.get_image_format(); }
  FrameData& get_current_frame() { return _frames[_frameNumber % FRAME_OVERLAP]; }
  VkExtent2D get_viewport_extent() const { return _viewportExtent; }
  const std::unordered_map<std::string, TargetInfo>& get_debug_targets() const { return _debugTargets; }

  bool is_using_hdr_environment() const;
  std::filesystem::path get_environment_path() const;
  UUID get_environment_hdr_uuid() const;

private:
  Window* _window{ nullptr };
  VulkanCore* _vulkanCore{ nullptr };
  ResourceManager* _resourceManager{ nullptr };
  Swapchain _swapchain;
  VkExtent2D _drawExtent{ 0, 0 };
  VkExtent2D _viewportExtent{ 1280, 720 };
  FrameData _frames[FRAME_OVERLAP];

  uint32_t _frameNumber{ 0 };
  uint32_t _swapchainImageIndex{ 0 };
  bool _isFrameStarted{ false };
  bool _frameFenceWaited{ false };
  bool _resizeRequested{ false };
  VkExtent2D _pendingResizeExtent{ 0, 0 };
  VkImageLayout _swapchainLayoutBeforePresent{ VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

  OverlayRenderCallback _overlayRenderCallback{ nullptr };
  SwapchainRecreateCallback _swapchainRecreateCallback{ nullptr };

  float _currentRenderScale{ 1.0f };

  std::vector<std::unique_ptr<RenderFeature>> _features{};
  EnvironmentPass* _environmentPass{ nullptr };

  std::unordered_map<std::string, TargetInfo> _debugTargets;
  RenderGraph _renderGraph{};
  RenderTargetPool _renderTargetPool;
  DrawCommandCache _drawCache;

  // frame rendering functions
  VkCommandBuffer begin_frame();
  void end_frame();

  // renderer initialization functions
  void create_command_buffers();
  void create_sync_structures();
  void update_render_extent();

  void extract_scene_data(const entt::registry& registry, DrawContext& ctx);
};
