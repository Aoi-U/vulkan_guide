file(GLOB_RECURSE RESOURCES_SOURCES 
    "*.cpp"
    "*.h"
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${RESOURCES_SOURCES})

add_library(resources STATIC ${RESOURCES_SOURCES})

target_include_directories(resources PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(resources PUBLIC
    core
    graphics
    fastgltf::fastgltf
    stb_image
	offsetallocator
	meshoptimizer
)