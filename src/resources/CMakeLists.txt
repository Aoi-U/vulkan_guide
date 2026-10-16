add_library(resources STATIC)

file(GLOB_RECURSE RESOURCES_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE RESOURCES_HEADERS CONFIGURE_DEPENDS 
	"${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

target_sources(resources PRIVATE
    ${RESOURCES_SOURCES}
    ${RESOURCES_HEADERS}
)

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
)