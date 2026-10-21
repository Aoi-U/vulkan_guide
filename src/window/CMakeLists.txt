file(GLOB_RECURSE WINDOW_SOURCES 
    "*.cpp"
    "*.h"
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${WINDOW_SOURCES})

add_library(window STATIC ${WINDOW_SOURCES})

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