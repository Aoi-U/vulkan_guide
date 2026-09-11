add_library(asset STATIC)

file(GLOB_RECURSE ASSET_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE ASSET_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.h")

target_sources(asset PRIVATE
    ${ASSET_SOURCES}
    ${ASSET_HEADERS}
)

target_include_directories(asset PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(asset PUBLIC
    core
    engine
    fastgltf::fastgltf
    stb_image
)