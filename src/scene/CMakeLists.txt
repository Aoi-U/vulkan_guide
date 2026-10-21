file(GLOB_RECURSE SCENE_SOURCES 
    "*.cpp"
    "*.h"
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${SCENE_SOURCES})

add_library(scene STATIC ${SCENE_SOURCES})

target_include_directories(scene PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(scene PUBLIC
    core
	resources
	graphics
	offsetallocator
)