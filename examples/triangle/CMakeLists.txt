find_program(CLEAN_GFX_SLANGC NAMES slangc REQUIRED)
find_program(CLEAN_GFX_SPIRV_VAL NAMES spirv-val)

set(CLEAN_GFX_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/triangle.slang)
set(CLEAN_GFX_SHADER_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(CLEAN_GFX_VERTEX_SPV ${CLEAN_GFX_SHADER_OUTPUT_DIR}/triangle.vert.spv)
set(CLEAN_GFX_FRAGMENT_SPV ${CLEAN_GFX_SHADER_OUTPUT_DIR}/triangle.frag.spv)

function(clean_gfx_compile_slang output entry stage)
    set(validate_command)
    if(CLEAN_GFX_SPIRV_VAL)
        set(validate_command COMMAND ${CLEAN_GFX_SPIRV_VAL}
            --target-env vulkan1.4 --scalar-block-layout ${output})
    endif()
    add_custom_command(
        OUTPUT ${output}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CLEAN_GFX_SHADER_OUTPUT_DIR}
        COMMAND ${CLEAN_GFX_SLANGC}
            ${CLEAN_GFX_SHADER_SOURCE}
            -target spirv
            -profile spirv_1_6
            -emit-spirv-directly
            -fvk-use-entrypoint-name
            -fvk-use-c-layout
            -I ${CMAKE_CURRENT_SOURCE_DIR}
            -I ${PROJECT_SOURCE_DIR}/include
            -entry ${entry}
            -stage ${stage}
            -o ${output}
        ${validate_command}
        DEPENDS
            ${CLEAN_GFX_SHADER_SOURCE}
            ${CMAKE_CURRENT_SOURCE_DIR}/triangle_shared.h
            ${PROJECT_SOURCE_DIR}/include/clean_gfx/shader_types.h
        VERBATIM
        COMMENT "Compiling Slang ${stage} shader ${entry}"
    )
endfunction()

clean_gfx_compile_slang(${CLEAN_GFX_VERTEX_SPV} vertexMain vertex)
clean_gfx_compile_slang(${CLEAN_GFX_FRAGMENT_SPV} fragmentMain fragment)

add_executable(clean_gfx_triangle
    triangle.cpp
    ../common/example_support.cpp
    ${CLEAN_GFX_VERTEX_SPV}
    ${CLEAN_GFX_FRAGMENT_SPV}
)
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
