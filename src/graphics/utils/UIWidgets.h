#pragma once

#include <glm/glm.hpp>
#include <string_view>

namespace UIWidgets {
  bool DrawVec3Control(
    std::string_view label,
    glm::vec3& values,
    const glm::vec3& defaultValues = glm::vec3(0.0f),
    float speed = 0.05f,
    bool* pLocked = nullptr
  );
}