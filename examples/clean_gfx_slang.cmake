function(clean_gfx_find_shader_tools)
    set(clean_gfx_minimum_slang_version 2026.14.1)
    set(clean_gfx_minimum_spirv_tools_version 2026.3)

    find_program(CLEAN_GFX_SLANGC NAMES slangc REQUIRED)
    find_program(CLEAN_GFX_SPIRV_VAL NAMES spirv-val REQUIRED)

    execute_process(
        COMMAND ${CLEAN_GFX_SLANGC} -version
        RESULT_VARIABLE clean_gfx_slang_version_result
        OUTPUT_VARIABLE clean_gfx_slang_version_output
        ERROR_VARIABLE clean_gfx_slang_version_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if(NOT clean_gfx_slang_version_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to query ${CLEAN_GFX_SLANGC} -version: "
            "${clean_gfx_slang_version_error}")
    endif()

    string(STRIP
        "${clean_gfx_slang_version_output}${clean_gfx_slang_version_error}"
        clean_gfx_slang_version_text)
    string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+"
        clean_gfx_slang_version "${clean_gfx_slang_version_text}")
    if(NOT clean_gfx_slang_version OR
       clean_gfx_slang_version VERSION_LESS clean_gfx_minimum_slang_version)
        message(FATAL_ERROR
            "clean_gfx examples require Slang ${clean_gfx_minimum_slang_version} "
            "or newer; found '${clean_gfx_slang_version_text}' at "
            "${CLEAN_GFX_SLANGC}")
    endif()

    execute_process(
        COMMAND ${CLEAN_GFX_SPIRV_VAL} --version
        RESULT_VARIABLE clean_gfx_spirv_tools_version_result
        OUTPUT_VARIABLE clean_gfx_spirv_tools_version_output
        ERROR_VARIABLE clean_gfx_spirv_tools_version_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if(NOT clean_gfx_spirv_tools_version_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to query ${CLEAN_GFX_SPIRV_VAL} --version: "
            "${clean_gfx_spirv_tools_version_error}")
    endif()

    string(STRIP
        "${clean_gfx_spirv_tools_version_output}${clean_gfx_spirv_tools_version_error}"
        clean_gfx_spirv_tools_version_text)
    string(REGEX MATCH "SPIRV-Tools v([0-9]+\\.[0-9]+)"
        clean_gfx_spirv_tools_version_match "${clean_gfx_spirv_tools_version_text}")
    set(clean_gfx_spirv_tools_version "${CMAKE_MATCH_1}")
    if(NOT clean_gfx_spirv_tools_version_match OR
       clean_gfx_spirv_tools_version VERSION_LESS clean_gfx_minimum_spirv_tools_version)
        message(FATAL_ERROR
            "clean_gfx examples require SPIRV-Tools ${clean_gfx_minimum_spirv_tools_version} "
            "or newer for SPV_EXT_descriptor_heap; found "
            "'${clean_gfx_spirv_tools_version_text}' at ${CLEAN_GFX_SPIRV_VAL}")
    endif()

    set(CLEAN_GFX_SLANGC ${CLEAN_GFX_SLANGC} PARENT_SCOPE)
    set(CLEAN_GFX_SPIRV_VAL ${CLEAN_GFX_SPIRV_VAL} PARENT_SCOPE)
endfunction()
