#pragma once

#include <glm/glm.hpp>
#include <string_view>
#include <core/UUID.h>
#include <resources/AssetMetadata.h>

static constexpr const char* IMGUI_PAYLOAD_TYPE_ASSET_UUID = "IMGUI_PAYLOAD_ASSET_UUID";

namespace UIWidgets {
  bool draw_vec3_control(
    std::string_view label,
    glm::vec3& values,
    const glm::vec3& defaultValues = glm::vec3(0.0f),
    float speed = 0.05f,
    bool* pLocked = nullptr
  );

  void draw_residency_badge(ResourceState state, uint64_t gpuSizeBytes = 0);

  bool draw_asset_slot(std::string_view label, UUID& assetUUID, AssetType expectedType, bool includeSubAssets = true);
}