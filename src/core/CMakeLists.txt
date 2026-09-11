add_library(core INTERFACE)

target_include_directories(core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src
)

target_compile_definitions(core INTERFACE GLM_FORCE_DEPTH_ZERO_TO_ONE)

target_link_libraries(core INTERFACE
    glm
    fmt::fmt
    Vulkan::Vulkan
    vma
)