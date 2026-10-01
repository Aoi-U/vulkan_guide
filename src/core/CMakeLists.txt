# Gather source and header files for compilation
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE CORE_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.h")

# Build core as a static library
add_library(core STATIC
    ${CORE_SOURCES}
    ${CORE_HEADERS}
)

# Keep include paths public so downstream targets (graphics, window, app) retain access
target_include_directories(core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src
)

target_compile_definitions(core PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE)

# Link dependencies needed by core and downstream modules
target_link_libraries(core PUBLIC
    glm
    fmt::fmt
    Vulkan::Vulkan
    vma
    entt
    imgui
)

# Optional IDE target to view headers cleanly in Visual Studio / Xcode
add_custom_target(core_ide_view ALL
    SOURCES ${CORE_HEADERS}
)
set_target_properties(core_ide_view PROPERTIES
    PROJECT_LABEL "core_ide"
)