#pragma once

#include <clean_gfx/shader_types.h>

struct CubeVertex
{
    float4 position;
    float2 uv;
};

struct CubeRootArguments
{
    CubeVertex* vertices;
    uint32 texture_index;
    uint32 sampler_index;
    float4 mvp[4];
};

#if !defined(CLEAN_GFX_SHADER) && !defined(__SLANG__)
#include <cstddef>
#include <type_traits>

static_assert(std::is_standard_layout_v<CubeVertex> &&
              std::is_trivially_copyable_v<CubeVertex>);
static_assert(sizeof(CubeVertex) == 24 && alignof(CubeVertex) == 4);
static_assert(offsetof(CubeVertex, position) == 0);
static_assert(offsetof(CubeVertex, uv) == 16);

static_assert(std::is_standard_layout_v<CubeRootArguments> &&
              std::is_trivially_copyable_v<CubeRootArguments>);
static_assert(std::is_same_v<decltype(CubeRootArguments::vertices), CubeVertex*>);
static_assert(sizeof(CubeRootArguments) == 80 && alignof(CubeRootArguments) == 8);
static_assert(offsetof(CubeRootArguments, vertices) == 0);
static_assert(offsetof(CubeRootArguments, texture_index) == 8);
static_assert(offsetof(CubeRootArguments, sampler_index) == 12);
static_assert(offsetof(CubeRootArguments, mvp) == 16);
#endif
