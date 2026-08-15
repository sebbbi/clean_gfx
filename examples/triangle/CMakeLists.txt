find_program(CLEAN_GFX_SLANGC NAMES slangc REQUIRED)

execute_process(
    COMMAND ${CLEAN_GFX_SLANGC} -h
    OUTPUT_VARIABLE CLEAN_GFX_SLANG_HELP_OUT
    ERROR_VARIABLE CLEAN_GFX_SLANG_HELP_ERR
)
set(CLEAN_GFX_SLANG_HELP "${CLEAN_GFX_SLANG_HELP_OUT}${CLEAN_GFX_SLANG_HELP_ERR}")

string(FIND "${CLEAN_GFX_SLANG_HELP}" "spvDescriptorHeapEXT" CLEAN_GFX_NATIVE_HEAP_POS)
if(CLEAN_GFX_NATIVE_HEAP_POS GREATER_EQUAL 0)
    set(CLEAN_GFX_NATIVE_HEAP TRUE)
    set(CLEAN_GFX_HEAP_ARGS
        -capability spvDescriptorHeapEXT
        -DCLEAN_GFX_NATIVE_DESCRIPTOR_HEAP=1
    )
    message(STATUS "Slang: native SPV_EXT_descriptor_heap code generation enabled")
else()
    set(CLEAN_GFX_NATIVE_HEAP FALSE)
    set(CLEAN_GFX_HEAP_ARGS -DCLEAN_GFX_DESCRIPTOR_MAPPING_FALLBACK=1)
    message(STATUS
        "Slang lacks spvDescriptorHeapEXT; using VK_EXT_descriptor_heap binding mappings")
endif()

string(FIND "${CLEAN_GFX_SLANG_HELP}" "-fvk-use-c-layout" CLEAN_GFX_C_LAYOUT_POS)
if(CLEAN_GFX_C_LAYOUT_POS GREATER_EQUAL 0)
    set(CLEAN_GFX_LAYOUT_ARG -fvk-use-c-layout)
else()
    set(CLEAN_GFX_LAYOUT_ARG -fvk-use-scalar-layout)
    message(WARNING
        "This Slang predates -fvk-use-c-layout. The sample root is explicitly padded, "
        "so scalar layout is ABI-compatible here; use Slang 2026.14.1+ for general shared structs.")
endif()

set(CLEAN_GFX_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/triangle.slang)
set(CLEAN_GFX_SHADER_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(CLEAN_GFX_VERTEX_SPV ${CLEAN_GFX_SHADER_OUTPUT_DIR}/triangle.vert.spv)
set(CLEAN_GFX_FRAGMENT_SPV ${CLEAN_GFX_SHADER_OUTPUT_DIR}/triangle.frag.spv)
set(CLEAN_GFX_COMPUTE_SPV ${CLEAN_GFX_SHADER_OUTPUT_DIR}/triangle.comp.spv)

find_program(CLEAN_GFX_SPIRV_VAL NAMES spirv-val)

function(clean_gfx_compile_slang output entry stage)
    set(validate_command)
    # SPIRV-Tools releases predating SPV_EXT_descriptor_heap reject the new
    # capability numerically even when the module is valid. Validate the
    # portable mapping fallback here; native modules are validated by a
    # current SDK/validation layer on the target PC.
    if(CLEAN_GFX_SPIRV_VAL AND NOT CLEAN_GFX_NATIVE_HEAP)
        set(validate_command COMMAND ${CLEAN_GFX_SPIRV_VAL} --target-env vulkan1.4 ${output})
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
            ${CLEAN_GFX_LAYOUT_ARG}
            ${CLEAN_GFX_HEAP_ARGS}
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
clean_gfx_compile_slang(${CLEAN_GFX_COMPUTE_SPV} computeMain compute)

add_executable(clean_gfx_triangle
    triangle.cpp
    ${CLEAN_GFX_VERTEX_SPV}
    ${CLEAN_GFX_FRAGMENT_SPV}
    ${CLEAN_GFX_COMPUTE_SPV}
)
target_link_libraries(clean_gfx_triangle PRIVATE clean_gfx::clean_gfx)
target_include_directories(clean_gfx_triangle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(clean_gfx_triangle PRIVATE
    CLEAN_GFX_VERTEX_SPV_PATH="${CLEAN_GFX_VERTEX_SPV}"
    CLEAN_GFX_FRAGMENT_SPV_PATH="${CLEAN_GFX_FRAGMENT_SPV}"
    CLEAN_GFX_COMPUTE_SPV_PATH="${CLEAN_GFX_COMPUTE_SPV}"
)
