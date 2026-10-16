find_package(Vulkan REQUIRED)

add_library(vkbootstrap STATIC)
add_library(glm INTERFACE)
add_library(vma INTERFACE)

add_library(stb_image INTERFACE)

add_library(entt INTERFACE)

add_library(stduuid INTERFACE)

add_subdirectory(fastgltf)
add_subdirectory(fmt EXCLUDE_FROM_ALL)
add_subdirectory(SDL EXCLUDE_FROM_ALL)

target_sources(vkbootstrap PRIVATE 
  vkbootstrap/VkBootstrap.h
  vkbootstrap/VkBootstrap.cpp
  )

target_include_directories(vkbootstrap PUBLIC vkbootstrap)
target_link_libraries(vkbootstrap PUBLIC Vulkan::Vulkan $<$<BOOL:UNIX>:${CMAKE_DL_LIBS}>)
set_property(TARGET vkbootstrap PROPERTY CXX_STANDARD 20)

#both vma and glm and header only libs so we only need the include path
target_include_directories(vma INTERFACE vma)

target_include_directories(glm INTERFACE glm)

#add_library(sdl2 INTERFACE)

#target_include_directories(sdl2 INTERFACE $ENV{VULKAN_SDK}/Include/SDL2 )
#target_link_directories(sdl2 INTERFACE $ENV{VULKAN_SDK}/Lib )

#target_link_libraries(sdl2 INTERFACE SDL2 SDL2main)

add_subdirectory(meshoptimizer)

add_library(offsetallocator STATIC)

target_sources(offsetallocator PRIVATE
    offsetallocator/offsetAllocator.hpp
    offsetallocator/offsetAllocator.cpp
)

target_include_directories(offsetallocator PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/offsetallocator
)

set_property(TARGET offsetallocator PROPERTY CXX_STANDARD 20)

add_library(yamlcpp INTERFACE)

include(FetchContent)

set(YAML_CPP_BUILD_TESTS OFF CACHE INTERNAL "")
set(YAML_CPP_BUILD_TOOLS OFF CACHE INTERNAL "")
set(YAML_CPP_BUILD_CONTRIB OFF CACHE INTERNAL "")

FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG yaml-cpp-0.9.0 # Can be a tag (yaml-cpp-x.x.x), a commit hash, or a branch name (master)
)
FetchContent_MakeAvailable(yaml-cpp)

target_link_libraries(yamlcpp INTERFACE yaml-cpp::yaml-cpp) # The library or executable that require yaml-cpp library

add_library(imgui STATIC)

add_library(spirv_reflect STATIC)

target_include_directories(imgui PUBLIC imgui)

target_sources(imgui PRIVATE 
  imgui/imgui.h
  imgui/imgui.cpp

  imgui/imgui_demo.cpp
  imgui/imgui_draw.cpp
  imgui/imgui_widgets.cpp
  imgui/imgui_tables.cpp
  imgui/imgui_impl_vulkan.cpp
  imgui/imgui_impl_sdl2.cpp
  imgui/ImGuizmo.h
  imgui/ImGuizmo.cpp
  )

target_sources(spirv_reflect PRIVATE
  spirv-reflect/spirv_reflect.h
  spirv-reflect/spirv_reflect.c
)

target_link_libraries(spirv_reflect PUBLIC Vulkan::Vulkan)

target_compile_definitions(spirv_reflect PUBLIC SPIRV_REFLECT_USE_SYSTEM_SPIRV_H)

target_link_libraries(imgui PUBLIC Vulkan::Vulkan SDL2::SDL2 spirv_reflect)

target_include_directories(stb_image INTERFACE stb_image)

target_include_directories(stduuid INTERFACE stduuid)

target_include_directories(spirv_reflect PUBLIC spirv-reflect
    "$ENV{VULKAN_SDK}/Include"
    ${Vulkan_INCLUDE_DIRS}
)

target_include_directories(entt INTERFACE entt)



