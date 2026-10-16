add_library(scene STATIC)

file(GLOB_RECURSE SCENE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE SCENE_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.h")

target_sources(scene PRIVATE
    ${SCENE_SOURCES}
    ${SCENE_HEADERS}
)

target_include_directories(scene PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(scene PUBLIC
    core
	resources
	graphics
)