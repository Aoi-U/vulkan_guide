add_library(graphics STATIC)

file(GLOB_RECURSE GRAPHICS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE GRAPHICS_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.h")

target_sources(graphics PRIVATE
    ${GRAPHICS_SOURCES}
    ${GRAPHICS_HEADERS}
)

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