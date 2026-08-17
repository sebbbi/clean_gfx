#pragma once

#if defined(__SLANG__)

// Slang already provides float[2-4], int[2-4], uint[2-4], float16_t[2-4],
// int16_t[2-4], uint16_t[2-4], and the native float3x4 matrix type.
// Redeclaring those types would hide the built-ins and can change shader semantics.
// Only add the short scalar aliases used by shared clean_gfx structures.
typedef uint8_t uint8;
typedef int8_t int8;
typedef uint16_t uint16;
typedef int16_t int16;
typedef uint32_t uint32;
typedef int32_t int32;
typedef uint64_t uint64;
typedef int64_t int64;

#else

#include <cstdint>

using uint8 = std::uint8_t;
using int8 = std::int8_t;
using uint16 = std::uint16_t;
using int16 = std::int16_t;
using uint32 = std::uint32_t;
using int32 = std::int32_t;
using uint64 = std::uint64_t;
using int64 = std::int64_t;

// CPU-side binary16 values are deliberately opaque storage. Conversion to or
// from float is kept out of the shared ABI layer so every bit pattern,
// including NaNs, crosses the CPU/GPU boundary unchanged.
struct float16_t
{
    uint16 bits;
};

struct float2
{
    float x;
    float y;
};

struct float3
{
    float x;
    float y;
    float z;
};

struct float4
{
    float x;
    float y;
    float z;
    float w;
};

// Slang names matrix dimensions as row-count x column-count.
struct float3x4
{
    float4 rows[3];
};

struct int2
{
    int32 x;
    int32 y;
};

struct int3
{
    int32 x;
    int32 y;
    int32 z;
};

struct int4
{
    int32 x;
    int32 y;
    int32 z;
    int32 w;
};

struct uint2
{
    uint32 x;
    uint32 y;
};

struct uint3
{
    uint32 x;
    uint32 y;
    uint32 z;
};

struct uint4
{
    uint32 x;
    uint32 y;
    uint32 z;
    uint32 w;
};

struct float16_t2
{
    float16_t x;
    float16_t y;
};

struct float16_t3
{
    float16_t x;
    float16_t y;
    float16_t z;
};

struct float16_t4
{
    float16_t x;
    float16_t y;
    float16_t z;
    float16_t w;
};

struct int16_t2
{
    int16 x;
    int16 y;
};

struct int16_t3
{
    int16 x;
    int16 y;
    int16 z;
};

struct int16_t4
{
    int16 x;
    int16 y;
    int16 z;
    int16 w;
};

struct uint16_t2
{
    uint16 x;
    uint16 y;
};

struct uint16_t3
{
    uint16 x;
    uint16 y;
    uint16 z;
};

struct uint16_t4
{
    uint16 x;
    uint16 y;
    uint16 z;
    uint16 w;
};

#endif
