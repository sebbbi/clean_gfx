#pragma once

// Slang defines __SLANG__. CLEAN_GFX_SHADER is also recognized so custom
// preprocessing pipelines can explicitly select the shader-facing branch.
#if defined(CLEAN_GFX_SHADER) || defined(__SLANG__)

// Slang already provides float[2-4], int[2-4], uint[2-4], float16_t[2-4],
// int16_t[2-4], and uint16_t[2-4]. Redeclaring those types would hide the native vectors
// and can change shader semantics. Only add the short scalar aliases used by
// shared clean_gfx structures.
typedef uint8_t uint8;
typedef int8_t int8;
typedef uint16_t uint16;
typedef int16_t int16;
typedef uint32_t uint32;
typedef int32_t int32;
typedef uint64_t uint64;
typedef int64_t int64;

// A shared struct uses this spelling for a device pointer. Slang sees a
// typed PhysicalStorageBuffer pointer; C++ stores the same bits as an opaque
// address and therefore cannot accidentally dereference GPU virtual memory.
#define CLEAN_GFX_DEVICE_PTR(type_) type_*

#else

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

using uint8 = std::uint8_t;
using int8 = std::int8_t;
using uint16 = std::uint16_t;
using int16 = std::int16_t;
using uint32 = std::uint32_t;
using int32 = std::int32_t;
using uint64 = std::uint64_t;
using int64 = std::int64_t;

#define CLEAN_GFX_DEVICE_PTR(type_) uint64

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

static_assert(std::endian::native == std::endian::little,
              "Slang shared data requires a little-endian CPU");
static_assert(sizeof(float) == 4 && alignof(float) == 4);
static_assert(std::numeric_limits<float>::is_iec559);
static_assert(std::numeric_limits<float>::digits == 24);
static_assert(std::numeric_limits<float>::max_exponent == 128);

static_assert(sizeof(uint8) == 1 && alignof(uint8) == 1);
static_assert(sizeof(int8) == 1 && alignof(int8) == 1);
static_assert(sizeof(uint16) == 2 && alignof(uint16) == 2);
static_assert(sizeof(int16) == 2 && alignof(int16) == 2);
static_assert(sizeof(uint32) == 4 && alignof(uint32) == 4);
static_assert(sizeof(int32) == 4 && alignof(int32) == 4);
static_assert(sizeof(uint64) == 8 && alignof(uint64) == 8);
static_assert(sizeof(int64) == 8 && alignof(int64) == 8);

#define CLEAN_GFX_ASSERT_SHARED_TYPE(type_, scalar_, component_count_)                        \
    static_assert(std::is_standard_layout_v<type_>);                                          \
    static_assert(std::is_trivial_v<type_>);                                                  \
    static_assert(std::is_trivially_copyable_v<type_>);                                       \
    static_assert(std::is_aggregate_v<type_>);                                                \
    static_assert(sizeof(type_) == sizeof(scalar_) * (component_count_));                     \
    static_assert(alignof(type_) == alignof(scalar_))

#define CLEAN_GFX_ASSERT_SHARED_VECTOR(type_, scalar_, component_count_)                      \
    CLEAN_GFX_ASSERT_SHARED_TYPE(type_, scalar_, component_count_);                           \
    static_assert(std::is_same_v<decltype(type_::x), scalar_>);                               \
    static_assert(offsetof(type_, x) == 0)

#define CLEAN_GFX_ASSERT_SHARED_VEC2(type_, scalar_)                                          \
    CLEAN_GFX_ASSERT_SHARED_VECTOR(type_, scalar_, 2);                                        \
    static_assert(std::is_same_v<decltype(type_::y), scalar_>);                               \
    static_assert(offsetof(type_, y) == sizeof(scalar_))

#define CLEAN_GFX_ASSERT_SHARED_VEC3(type_, scalar_)                                          \
    CLEAN_GFX_ASSERT_SHARED_VECTOR(type_, scalar_, 3);                                        \
    static_assert(std::is_same_v<decltype(type_::y), scalar_>);                               \
    static_assert(std::is_same_v<decltype(type_::z), scalar_>);                               \
    static_assert(offsetof(type_, y) == sizeof(scalar_));                                     \
    static_assert(offsetof(type_, z) == sizeof(scalar_) * 2)

#define CLEAN_GFX_ASSERT_SHARED_VEC4(type_, scalar_)                                          \
    CLEAN_GFX_ASSERT_SHARED_VECTOR(type_, scalar_, 4);                                        \
    static_assert(std::is_same_v<decltype(type_::y), scalar_>);                               \
    static_assert(std::is_same_v<decltype(type_::z), scalar_>);                               \
    static_assert(std::is_same_v<decltype(type_::w), scalar_>);                               \
    static_assert(offsetof(type_, y) == sizeof(scalar_));                                     \
    static_assert(offsetof(type_, z) == sizeof(scalar_) * 2);                                 \
    static_assert(offsetof(type_, w) == sizeof(scalar_) * 3)

CLEAN_GFX_ASSERT_SHARED_TYPE(float16_t, uint16, 1);
static_assert(std::is_same_v<decltype(float16_t::bits), uint16>);
static_assert(offsetof(float16_t, bits) == 0);

CLEAN_GFX_ASSERT_SHARED_VEC2(float2, float);
CLEAN_GFX_ASSERT_SHARED_VEC3(float3, float);
CLEAN_GFX_ASSERT_SHARED_VEC4(float4, float);
CLEAN_GFX_ASSERT_SHARED_VEC2(int2, int32);
CLEAN_GFX_ASSERT_SHARED_VEC3(int3, int32);
CLEAN_GFX_ASSERT_SHARED_VEC4(int4, int32);
CLEAN_GFX_ASSERT_SHARED_VEC2(uint2, uint32);
CLEAN_GFX_ASSERT_SHARED_VEC3(uint3, uint32);
CLEAN_GFX_ASSERT_SHARED_VEC4(uint4, uint32);
CLEAN_GFX_ASSERT_SHARED_VEC2(float16_t2, float16_t);
CLEAN_GFX_ASSERT_SHARED_VEC3(float16_t3, float16_t);
CLEAN_GFX_ASSERT_SHARED_VEC4(float16_t4, float16_t);
CLEAN_GFX_ASSERT_SHARED_VEC2(int16_t2, int16);
CLEAN_GFX_ASSERT_SHARED_VEC3(int16_t3, int16);
CLEAN_GFX_ASSERT_SHARED_VEC4(int16_t4, int16);
CLEAN_GFX_ASSERT_SHARED_VEC2(uint16_t2, uint16);
CLEAN_GFX_ASSERT_SHARED_VEC3(uint16_t3, uint16);
CLEAN_GFX_ASSERT_SHARED_VEC4(uint16_t4, uint16);

#undef CLEAN_GFX_ASSERT_SHARED_VEC4
#undef CLEAN_GFX_ASSERT_SHARED_VEC3
#undef CLEAN_GFX_ASSERT_SHARED_VEC2
#undef CLEAN_GFX_ASSERT_SHARED_VECTOR
#undef CLEAN_GFX_ASSERT_SHARED_TYPE

#endif
