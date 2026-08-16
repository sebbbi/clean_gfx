find_program(CLEAN_GFX_SLANGC NAMES slangc REQUIRED)
find_program(CLEAN_GFX_SPIRV_VAL NAMES spirv-val)

set(CLEAN_GFX_CUBE_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/cube.slang)
set(CLEAN_GFX_CUBE_SHADER_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(CLEAN_GFX_CUBE_VERTEX_SPV ${CLEAN_GFX_CUBE_SHADER_OUTPUT_DIR}/cube.vert.spv)
set(CLEAN_GFX_CUBE_FRAGMENT_SPV ${CLEAN_GFX_CUBE_SHADER_OUTPUT_DIR}/cube.frag.spv)

execute_process(
    COMMAND ${CLEAN_GFX_SLANGC} -h
    OUTPUT_VARIABLE CLEAN_GFX_CUBE_SLANG_HELP
    ERROR_VARIABLE CLEAN_GFX_CUBE_SLANG_HELP_ERROR
)
string(APPEND CLEAN_GFX_CUBE_SLANG_HELP "${CLEAN_GFX_CUBE_SLANG_HELP_ERROR}")

set(CLEAN_GFX_CUBE_DESCRIPTOR_ARGUMENTS)
set(CLEAN_GFX_CUBE_NATIVE_DESCRIPTOR_HEAP OFF)
if(CLEAN_GFX_CUBE_SLANG_HELP MATCHES "spvDescriptorHeapEXT")
    set(CLEAN_GFX_CUBE_NATIVE_DESCRIPTOR_HEAP ON)
    list(APPEND CLEAN_GFX_CUBE_DESCRIPTOR_ARGUMENTS
        -capability spvDescriptorHeapEXT
        -DCLEAN_GFX_NATIVE_DESCRIPTOR_HEAP=1)
else()
    list(APPEND CLEAN_GFX_CUBE_DESCRIPTOR_ARGUMENTS
        -DCLEAN_GFX_DESCRIPTOR_MAPPING_FALLBACK=1)
endif()

set(CLEAN_GFX_CUBE_LAYOUT_ARGUMENT -fvk-use-c-layout)
if(NOT CLEAN_GFX_CUBE_SLANG_HELP MATCHES "fvk-use-c-layout")
    set(CLEAN_GFX_CUBE_LAYOUT_ARGUMENT -fvk-use-scalar-layout)
    message(WARNING
        "slangc does not advertise -fvk-use-c-layout; the cube example is using "
        "the scalar-layout compatibility path")
endif()

function(clean_gfx_compile_cube_slang output entry stage)
    set(validate_command)
    if(CLEAN_GFX_SPIRV_VAL AND NOT CLEAN_GFX_CUBE_NATIVE_DESCRIPTOR_HEAP)
        set(validate_command COMMAND ${CLEAN_GFX_SPIRV_VAL}
            --target-env vulkan1.4 --scalar-block-layout ${output})
    endif()
    add_custom_command(
        OUTPUT ${output}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CLEAN_GFX_CUBE_SHADER_OUTPUT_DIR}
        COMMAND ${CLEAN_GFX_SLANGC}
            ${CLEAN_GFX_CUBE_SHADER_SOURCE}
            -target spirv
            -profile spirv_1_6
            -emit-spirv-directly
            -fvk-use-entrypoint-name
            ${CLEAN_GFX_CUBE_LAYOUT_ARGUMENT}
            ${CLEAN_GFX_CUBE_DESCRIPTOR_ARGUMENTS}
            -I ${CMAKE_CURRENT_SOURCE_DIR}
            -I ${PROJECT_SOURCE_DIR}/include
            -entry ${entry}
            -stage ${stage}
            -o ${output}
        ${validate_command}
        DEPENDS
            ${CLEAN_GFX_CUBE_SHADER_SOURCE}
            ${CMAKE_CURRENT_SOURCE_DIR}/cube_shared.h
            ${PROJECT_SOURCE_DIR}/include/clean_gfx/shader_types.h
        VERBATIM
        COMMENT "Compiling cube Slang ${stage} shader ${entry}"
    )
endfunction()

clean_gfx_compile_cube_slang(${CLEAN_GFX_CUBE_VERTEX_SPV} vertexMain vertex)
clean_gfx_compile_cube_slang(${CLEAN_GFX_CUBE_FRAGMENT_SPV} fragmentMain fragment)

add_executable(clean_gfx_cube
    cube.cpp
    ../common/example_support.cpp
    ${CLEAN_GFX_CUBE_VERTEX_SPV}
    ${CLEAN_GFX_CUBE_FRAGMENT_SPV}
)
clean_gfx_disable_exceptions(clean_gfx_cube)
target_link_libraries(clean_gfx_cube PRIVATE clean_gfx::clean_gfx)
target_include_directories(clean_gfx_cube PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
)
target_compile_definitions(clean_gfx_cube PRIVATE
    CLEAN_GFX_CUBE_VERTEX_SPV_PATH="${CLEAN_GFX_CUBE_VERTEX_SPV}"
    CLEAN_GFX_CUBE_FRAGMENT_SPV_PATH="${CLEAN_GFX_CUBE_FRAGMENT_SPV}"
)

if(WIN32)
    target_link_libraries(clean_gfx_cube PRIVATE
        user32 gdi32 windowscodecs ole32)
endif()

if(CMAKE_GENERATOR MATCHES "^Visual Studio")
    set_property(TARGET clean_gfx_cube PROPERTY
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    if(DEFINED ENV{VULKAN_SDK})
        set_property(TARGET clean_gfx_cube PROPERTY
            VS_DEBUGGER_ENVIRONMENT
                "VK_LAYER_PATH=$ENV{VULKAN_SDK}/Bin\nVK_LOADER_LAYERS_DISABLE=~implicit~")
    endif()
endif()
