#pragma once

#include <clean_gfx/shader_types.h>

struct Vertex
{
    float3 position;
    float3 color;
};

struct RootArguments
{
    Vertex* vertices;
};

#if !defined(CLEAN_GFX_SHADER) && !defined(__SLANG__)
#include <cstddef>
#include <type_traits>

static_assert(std::is_standard_layout_v<Vertex> && std::is_trivially_copyable_v<Vertex>);
static_assert(sizeof(Vertex) == 24 && alignof(Vertex) == 4);
static_assert(offsetof(Vertex, position) == 0);
static_assert(offsetof(Vertex, color) == 12);

static_assert(std::is_standard_layout_v<RootArguments> &&
              std::is_trivially_copyable_v<RootArguments>);
static_assert(std::is_same_v<decltype(RootArguments::vertices), Vertex*>);
static_assert(sizeof(Vertex*) == 8 && alignof(Vertex*) == 8);
static_assert(sizeof(RootArguments) == 8 && alignof(RootArguments) == 8);
static_assert(offsetof(RootArguments, vertices) == 0);
#endif

