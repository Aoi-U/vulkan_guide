# 1. Real header-only interface library for CMake linking
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

# 2. Visual Studio target that displays vk_types.h without compiling anything
file(GLOB_RECURSE CORE_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.h")

add_custom_target(core_ide_view ALL
    SOURCES ${CORE_HEADERS}
)

set_target_properties(core_ide_view PROPERTIES
    PROJECT_LABEL "core"
)