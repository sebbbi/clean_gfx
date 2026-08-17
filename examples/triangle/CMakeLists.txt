include(${CMAKE_CURRENT_LIST_DIR}/../clean_gfx_slang.cmake)
clean_gfx_find_shader_tools()

set(CLEAN_GFX_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/triangle.slang)
set(CLEAN_GFX_SHADER_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(CLEAN_GFX_VERTEX_SPV ${CLEAN_GFX_SHADER_OUTPUT_DIR}/triangle.vert.spv)
set(CLEAN_GFX_FRAGMENT_SPV ${CLEAN_GFX_SHADER_OUTPUT_DIR}/triangle.frag.spv)

function(clean_gfx_compile_slang output entry stage)
    add_custom_command(
        OUTPUT ${output}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CLEAN_GFX_SHADER_OUTPUT_DIR}
        COMMAND ${CLEAN_GFX_SLANGC}
            ${CLEAN_GFX_SHADER_SOURCE}
            -target spirv
            -profile spirv_1_6
            -emit-spirv-directly
            -fvk-use-entrypoint-name
            -entry ${entry}
            -stage ${stage}
            -o ${output}
        COMMAND ${CLEAN_GFX_SPIRV_VAL}
            --target-env vulkan1.4 --scalar-block-layout ${output}
        DEPENDS
            ${CLEAN_GFX_SHADER_SOURCE}
        VERBATIM
        COMMENT "Compiling Slang ${stage} shader ${entry}"
    )
endfunction()

clean_gfx_compile_slang(${CLEAN_GFX_VERTEX_SPV} vertexMain vertex)
clean_gfx_compile_slang(${CLEAN_GFX_FRAGMENT_SPV} fragmentMain fragment)

add_executable(clean_gfx_triangle
    triangle.cpp
    ${CLEAN_GFX_SHADER_SOURCE}
    ../common/example_support.cpp
    ../common/example_support.hpp
    ${CLEAN_GFX_VERTEX_SPV}
    ${CLEAN_GFX_FRAGMENT_SPV}
)
set_source_files_properties(${CLEAN_GFX_SHADER_SOURCE}
    PROPERTIES HEADER_FILE_ONLY TRUE)
source_group("Shaders" FILES ${CLEAN_GFX_SHADER_SOURCE})
source_group("Common" FILES
    ../common/example_support.cpp
    ../common/example_support.hpp)
clean_gfx_disable_exceptions(clean_gfx_triangle)
target_link_libraries(clean_gfx_triangle PRIVATE clean_gfx::clean_gfx)
target_include_directories(clean_gfx_triangle PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
)
target_compile_definitions(clean_gfx_triangle PRIVATE
    CLEAN_GFX_VERTEX_SPV_PATH="${CLEAN_GFX_VERTEX_SPV}"
    CLEAN_GFX_FRAGMENT_SPV_PATH="${CLEAN_GFX_FRAGMENT_SPV}"
)

if(WIN32)
    target_link_libraries(clean_gfx_triangle PRIVATE user32 gdi32)
endif()

if(CMAKE_GENERATOR MATCHES "^Visual Studio")
    set_property(TARGET clean_gfx_triangle PROPERTY
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    if(DEFINED ENV{VULKAN_SDK})
        set_property(TARGET clean_gfx_triangle PROPERTY
            VS_DEBUGGER_ENVIRONMENT
                "VK_LAYER_PATH=$ENV{VULKAN_SDK}/Bin\nVK_LOADER_LAYERS_DISABLE=~implicit~")
    endif()
endif()
