#pragma once

#include <clean_gfx/shader_types.h>

struct Vertex
{
    float2 position;
    float2 uv;
};

struct RootArguments
{
    CLEAN_GFX_DEVICE_PTR(Vertex) vertices;
    uint32 texture_index;
    uint32 sampler_index;
    float exposure;
    float16_t2 tint_rg;
    float16_t2 tint_ba;
    uint32 padding;
};

#if !defined(CLEAN_GFX_SHADER) && !defined(__SLANG__)
#include <cstddef>
#include <type_traits>

static_assert(std::is_standard_layout_v<Vertex> && std::is_trivially_copyable_v<Vertex>);
static_assert(sizeof(Vertex) == 16 && alignof(Vertex) == 4);
static_assert(offsetof(Vertex, position) == 0);
static_assert(offsetof(Vertex, uv) == 8);

static_assert(std::is_standard_layout_v<RootArguments> &&
              std::is_trivially_copyable_v<RootArguments>);
static_assert(sizeof(RootArguments) == 32 && alignof(RootArguments) == 8);
static_assert(offsetof(RootArguments, vertices) == 0);
static_assert(offsetof(RootArguments, texture_index) == 8);
static_assert(offsetof(RootArguments, sampler_index) == 12);
static_assert(offsetof(RootArguments, exposure) == 16);
static_assert(offsetof(RootArguments, tint_rg) == 20);
static_assert(offsetof(RootArguments, tint_ba) == 24);
static_assert(offsetof(RootArguments, padding) == 28);
#endif

