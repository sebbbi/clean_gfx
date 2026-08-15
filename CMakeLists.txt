cmake_minimum_required(VERSION 3.24)

project(clean_gfx VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)

option(CLEAN_GFX_BUILD_EXAMPLES "Build clean_gfx examples" ON)
option(CLEAN_GFX_BUILD_TESTS "Build clean_gfx tests" ON)
option(CLEAN_GFX_FETCH_VULKAN_HEADERS "Fetch Vulkan-Headers new enough for descriptor heaps" ON)

if(CLEAN_GFX_FETCH_VULKAN_HEADERS)
    set(VULKAN_HEADERS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(VULKAN_HEADERS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        VulkanHeaders
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG v1.4.357
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(VulkanHeaders)
    find_package(Vulkan REQUIRED)
else()
    find_package(Vulkan REQUIRED)
    if(Vulkan_VERSION VERSION_LESS 1.4.357)
        message(FATAL_ERROR
            "clean_gfx requires Vulkan-Headers 1.4.357 or newer. "
            "Enable CLEAN_GFX_FETCH_VULKAN_HEADERS or install a current Vulkan SDK.")
    endif()
    if(NOT TARGET Vulkan::Headers)
        add_library(clean_gfx_system_vulkan_headers INTERFACE)
        target_include_directories(clean_gfx_system_vulkan_headers INTERFACE ${Vulkan_INCLUDE_DIRS})
        add_library(Vulkan::Headers ALIAS clean_gfx_system_vulkan_headers)
    endif()
endif()

add_library(clean_gfx STATIC
    src/clean_gfx.cpp
)
add_library(clean_gfx::clean_gfx ALIAS clean_gfx)

target_compile_features(clean_gfx PUBLIC cxx_std_20)
target_include_directories(clean_gfx
    BEFORE PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(clean_gfx PUBLIC Vulkan::Headers PRIVATE Vulkan::Vulkan)

if(MSVC)
    target_compile_options(clean_gfx PRIVATE /W4 /permissive-)
else()
    target_compile_options(clean_gfx PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(CLEAN_GFX_BUILD_EXAMPLES)
    add_subdirectory(examples/triangle)
endif()

if(CLEAN_GFX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
