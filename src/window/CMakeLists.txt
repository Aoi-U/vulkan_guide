add_library(window STATIC)

file(GLOB_RECURSE WINDOW_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE WINDOW_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.h")

target_sources(window PRIVATE
    ${WINDOW_SOURCES}
    ${WINDOW_HEADERS}
)

target_include_directories(window PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(window PUBLIC
    core
    SDL2::SDL2
    Vulkan::Vulkan
    imgui
)