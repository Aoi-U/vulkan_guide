file(GLOB_RECURSE GRAPHICS_SOURCES 
    "*.cpp"
    "*.h"
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${GRAPHICS_SOURCES})

add_library(graphics STATIC ${GRAPHICS_SOURCES})

target_include_directories(graphics PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(graphics PUBLIC
    core
    window
    resources
    Vulkan::Vulkan
    vma
    vkbootstrap
    SDL2::SDL2
    imgui
    fmt::fmt
    glm
	offsetallocator
)

add_dependencies(graphics Shaders)