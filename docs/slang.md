# Slang shader contract

`clean_gfx` uses Slang as the source language for a Vulkan-only binding model:

- a small C-compatible root structure is written with `vkCmdPushDataEXT`;
- buffers and larger structures are reached through typed buffer device address
  (BDA) pointers in that root structure; and
- textures and samplers are selected by integer indices into the
  `VK_EXT_descriptor_heap` resource and sampler heaps.

There are no application-visible descriptor sets, descriptor pools, or pipeline
layouts in either shader compilation path described below.

## Supported Slang version

Use **Slang v2026.14.1 or newer** for the native path.

Slang v2026.14 added direct `ResourceDescriptorHeap` and
`SamplerDescriptorHeap` input syntax. The initial v2026.14.0 release had a
descriptor-heap `ConstantBuffer<T>` lowering bug: it emitted a storage-buffer
descriptor instead of a uniform-buffer descriptor. That bug was fixed in
v2026.14.1. The build detects the capability rather than parsing the compiler
version, so it is the developer's responsibility not to use v2026.14.0.

- [Direct descriptor-heap input support](https://github.com/shader-slang/slang/pull/11798)
- [v2026.14 `ConstantBuffer` issue](https://github.com/shader-slang/slang/issues/12226)
- [v2026.14.1 fix](https://github.com/shader-slang/slang/pull/12256)
- [Slang releases](https://github.com/shader-slang/slang/releases)

## Exact compiler options

[`examples/triangle/CMakeLists.txt`](../examples/triangle/CMakeLists.txt) invokes
the native compiler path equivalently to the following command for the vertex
entry point:

```sh
slangc examples/triangle/triangle.slang \
  -target spirv \
  -profile spirv_1_6 \
  -emit-spirv-directly \
  -fvk-use-entrypoint-name \
  -fvk-use-c-layout \
  -capability spvDescriptorHeapEXT \
  -DCLEAN_GFX_NATIVE_DESCRIPTOR_HEAP=1 \
  -I examples/triangle \
  -I include \
  -entry vertexMain \
  -stage vertex \
  -o triangle.vert.spv
```

The fragment invocation changes the entry point, stage, and output to
`fragmentMain`, `fragment`, and `triangle.frag.spv`; the compute invocation uses
`computeMain`, `compute`, and `triangle.comp.spv`.

The significant options are:

- `-target spirv -profile spirv_1_6` targets the SPIR-V version supported by the
  Vulkan 1.4 baseline used by this repository.
- `-emit-spirv-directly` selects Slang's direct SPIR-V backend explicitly.
- `-fvk-use-entrypoint-name` preserves names such as `vertexMain`; pipeline
  creation uses those names rather than `main`.
- `-fvk-use-c-layout` makes the shader representation agree with the C++ POD
  representation described below.
- `-capability spvDescriptorHeapEXT` enables native
  `SPV_EXT_descriptor_heap` lowering. In Slang's capability definitions this
  also brings in `SPV_KHR_untyped_pointers`.
- `-DCLEAN_GFX_NATIVE_DESCRIPTOR_HEAP=1` selects direct heap syntax in the
  example source.

The current resource heap contains only image descriptors and uses
`VkPhysicalDeviceDescriptorHeapPropertiesEXT::imageDescriptorSize`; samplers
have a separate heap. Consequently the build does not use
`-spirv-unified-descriptor-heap-stride`. If buffer descriptors or other
different-sized resource descriptors are later placed in the same numeric slot
space, use that option or define explicit per-type index/stride semantics. The
related Slang options are `-spirv-resource-heap-stride`,
`-spirv-sampler-heap-stride`, and
`-spirv-unified-descriptor-heap-stride`.

Primary references:

- [Slang command-line reference](https://github.com/shader-slang/slang/blob/master/docs/command-line-slangc-reference.md)
- [Slang capability definition](https://github.com/shader-slang/slang/blob/master/source/slang/slang-capabilities.capdef)
- [`SPV_EXT_descriptor_heap`](https://github.khronos.org/SPIRV-Registry/extensions/EXT/SPV_EXT_descriptor_heap.html)

## Native descriptor-heap syntax

The native branch in
[`examples/triangle/triangle.slang`](../examples/triangle/triangle.slang) is:

```slang
Texture2D<float4> texture = ResourceDescriptorHeap[root.texture_index];
SamplerState sampler = SamplerDescriptorHeap[root.sampler_index];
RWTexture2D<float4> output = ResourceDescriptorHeap[root.texture_index];
```

`Texture::sampled_index()` and `Texture::storage_index()` return resource-heap
indices. `Sampler::index()` returns a sampler-heap index. Resource and sampler
indices are therefore different kinds of values even though both are stored as
`uint32`.

Slang can also materialize a resource-specific `.Handle` from a heap access for
storage or passing through user code. Internally descriptor handles use a
`uint2` representation. A combined texture/sampler handle uses `.x` for the
resource heap and `.y` for the sampler heap.

Under `SPV_EXT_descriptor_heap`, dynamically selected heap descriptors are
non-uniform by default. No conventional descriptor-set declarations or
`NonUniform` decoration are needed for legality.

See [Slang direct descriptor-heap indexing](https://github.com/shader-slang/slang/blob/master/docs/user-guide/03-convenience-features.md#direct-descriptor-heap-indexing)
and [the SPIR-V-specific lowering](https://github.com/shader-slang/slang/blob/master/docs/user-guide/03-convenience-features.md#spir-v-with-spvdescriptorheapext).

## Automatic older-Slang mapping fallback

The fallback is a **compiler compatibility path**, not a runtime fallback from
`VK_EXT_descriptor_heap`.

At CMake configure time, the triangle build runs `slangc -h` and searches its
output for `spvDescriptorHeapEXT`:

1. If present, CMake passes `-capability spvDescriptorHeapEXT` and defines
   `CLEAN_GFX_NATIVE_DESCRIPTOR_HEAP`.
2. If absent, CMake defines `CLEAN_GFX_DESCRIPTOR_MAPPING_FALLBACK` instead.

The older-compiler branch declares conventional unsized descriptor arrays:

```slang
#if defined(CLEAN_GFX_DESCRIPTOR_MAPPING_FALLBACK)
[[vk::binding(0, 0)]] Texture2D<float4> mapped_textures[];
[[vk::binding(1, 0)]] SamplerState mapped_samplers[];
[[vk::binding(2, 0)]] RWTexture2D<float4> mapped_storage_textures[];
#endif

// ...

Texture2D<float4> texture = mapped_textures[root.texture_index];
SamplerState sampler = mapped_samplers[root.sampler_index];
RWTexture2D<float4> output = mapped_storage_textures[root.texture_index];
```

During Vulkan pipeline creation, `clean_gfx` chains
`VkShaderDescriptorSetAndBindingMappingInfoEXT` to each shader stage. The
standard mapping sends set 0, binding 0 sampled images to the resource heap,
set 0, binding 1 samplers to the sampler heap, and set 0, binding 2 read/write
images to the resource heap, with device-reported descriptor sizes as array
strides. The sample exercises all three mappings. The indices returned by the
public API therefore have the same meaning in native and mapped shaders.

This still creates a descriptor-heap pipeline with a null pipeline layout and
still writes and binds `VK_EXT_descriptor_heap` heaps. It does not create a
descriptor set or pool, and it does not make the program work on a device that
lacks the required Vulkan extensions. The runtime enables
`runtimeDescriptorArray` for this path.

Layout detection is independent. If an old compiler does not advertise
`-fvk-use-c-layout`, CMake uses `-fvk-use-scalar-layout` and prints a warning.
The triangle's shared structures are explicitly padded so those two layouts
happen to agree for that sample. Scalar layout is not a general C ABI fallback;
new shared structures should be built with Slang v2026.14.1 or newer and
`-fvk-use-c-layout`.

If `spirv-val` is installed, CMake validates mapped-fallback modules against
Vulkan 1.4. It currently leaves native modules to a current target SDK and
validation layer because older SPIRV-Tools builds reject the new extension's
capability number even when the module is valid.

Descriptor mappings are specified by
[`VK_EXT_descriptor_heap`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_descriptor_heap.html).

## Root data and `vkCmdPushDataEXT`

`vkCmdPushDataEXT` updates the ordinary SPIR-V `PushConstant` storage class. It
does not need a new Slang storage kind. Declare one root object as follows:

```slang
[[vk::push_constant]]
ConstantBuffer<RootArguments> root;
```

The corresponding C++ value can be submitted with:

```cpp
commands.push_root(root_arguments);
```

`CommandList::push_root<T>` verifies that `T` is standard-layout and trivially copyable, that its
size is a multiple of four, and forwards
its bytes to `CommandList::push_data`, which records `vkCmdPushDataEXT`.
`push_data` also exposes a byte offset for partial updates. The implementation
requires the offset and size to be multiples of four and rejects writes beyond
`DeviceCaps::max_push_data_size`.

Push data is CPU-sourced. There is no form that asks `vkCmdPushDataEXT` to fetch
the root object from a GPU address. Store BDA pointers in the root to reach
larger GPU-resident data. Avoid mixing this model with legacy
`vkCmdPushConstants` or push descriptors because the Vulkan commands invalidate
one another's push-data state.

- [Slang push constants](https://github.com/shader-slang/slang/blob/master/docs/user-guide/a2-01-spirv-target-specific.md#push-constants)
- [`VK_EXT_descriptor_heap` proposal, including push data](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_EXT_descriptor_heap.adoc)

## Buffer device address pointers

Shared structures spell a device pointer with `CLEAN_GFX_DEVICE_PTR(T)`:

```c
struct RootArguments
{
    CLEAN_GFX_DEVICE_PTR(Vertex) vertices;
    uint32 texture_index;
    uint32 sampler_index;
    // ...
};
```

[`include/clean_gfx/shader_types.h`](../include/clean_gfx/shader_types.h)
expands the macro differently on each side:

```c
// Slang
#define CLEAN_GFX_DEVICE_PTR(type_) type_*

// C++
#define CLEAN_GFX_DEVICE_PTR(type_) uint64
```

Thus Slang sees a typed `Vertex*`, while C++ stores the opaque 64-bit value
returned by `Buffer::address()`. It deliberately never exposes that value as a
dereferenceable host pointer. The shader can use normal typed access:

```slang
Vertex vertex = root.vertices[vertex_id];
```

Slang lowers such pointers to the SPIR-V `PhysicalStorageBuffer` storage class,
the `PhysicalStorageBufferAddresses` capability, and the
`PhysicalStorageBuffer64` addressing model. A more explicit shader-only spelling
is `Ptr<T, Access.Read, AddressSpace.Device>`, but the native `T*` spelling keeps
the shared header simple.

The Vulkan runtime enables `bufferDeviceAddress`, creates buffers with
`VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`, allocates their memory for device
addresses, and obtains each address with `vkGetBufferDeviceAddress`.

Important pointer limitations are:

- a BDA pointer has no bound, so robust buffer access cannot make an out-of-range
  dereference safe;
- the application must keep the allocation alive and satisfy the pointee's
  alignment;
- opaque resources such as textures and samplers cannot be pointees—use heap
  indices for them;
- Slang's Vulkan pointer support is intentionally smaller than C++ pointer
  semantics, including restrictions on pointers to local variables, `const`
  syntax, custom alignment, and inheritance; and
- storing the address as `uint64_t` and casting it in shader code can introduce
  a `shaderInt64` requirement. A typed pointer field does not require that
  integer feature merely to carry the address.

- [Slang pointer support and limitations](https://github.com/shader-slang/slang/blob/master/docs/user-guide/03-convenience-features.md#pointers-limited)
- [Slang SPIR-V global-memory pointers](https://github.com/shader-slang/slang/blob/master/docs/user-guide/a2-01-spirv-target-specific.md#global-memory-pointers)
- [`SPV_KHR_physical_storage_buffer`](https://github.khronos.org/SPIRV-Registry/extensions/KHR/SPV_KHR_physical_storage_buffer.html)
- [Vulkan buffer device address guide](https://docs.vulkan.org/guide/latest/buffer_device_address.html)
- [BDA alignment requirements](https://docs.vulkan.org/guide/latest/buffer_device_address_alignment.html)

## Shared C/C++ POD layout

Shader and CPU code both include the same application structure header, as the
triangle does with
[`examples/triangle/triangle_shared.h`](../examples/triangle/triangle_shared.h).
That header first includes `<clean_gfx/shader_types.h>`.

Slang predefines `__SLANG__`. `shader_types.h` also accepts
`CLEAN_GFX_SHADER` for custom preprocessing pipelines. On the shader branch it
uses Slang's native `float2`, `float3`, `float4`, `int2`, `uint4`,
`float16_t2`, `int16_t3`, `uint16_t4`, and similar types. On the C++ branch it declares scalar-member POD
types with the same names and verifies their size, alignment, member offsets,
triviality, IEEE-754 float representation, and little-endian host byte order.

`-fvk-use-c-layout` is required because it applies C/C++ layout rules to values
accessed through `ConstantBuffer`, `ParameterBlock`, `StructuredBuffer`,
`ByteAddressBuffer`, and general pointers. `-fvk-use-scalar-layout` is close but
not identical: C layout also rounds a nested structure's total size up to its
alignment. That difference changes the following member's offset in arrays and
outer structures.

The runtime requires and enables Vulkan `scalarBlockLayout`. Keep the following
rules for every shared structure:

- use the scalar-member types supplied by `shader_types.h`, not host SIMD types
  with platform-specific alignment;
- add C++ `sizeof`, `alignof`, and `offsetof` assertions for application
  structures, following `triangle_shared.h`;
- make padding explicit when the ABI is externally visible;
- avoid matrices in shared roots unless row/column-major order and the CPU
  representation are fixed explicitly; and
- keep C++ native pointers, references, containers, constructors, and virtual
  members out of shared structures.

Primary references:

- [`-fvk-use-c-layout`](https://github.com/shader-slang/slang/blob/master/docs/command-line-slangc-reference.md#fvk-use-c-layout)
- [Slang C-layout regression test](https://github.com/shader-slang/slang/blob/master/tests/spirv/c-layout-buffer.slang)
- [Slang's `__SLANG__` predefined macro](https://github.com/shader-slang/slang/blob/master/source/slang/slang-translation-unit.cpp)
- [Vulkan scalar block layout](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_scalar_block_layout.html)

## 16-bit values

Slang's true 16-bit scalar and vector names include `float16_t`, `float16_t2`,
`int16_t`, `uint16_t`, and `uint16_t4`. The C++ `float16_t` in
`shader_types.h` is deliberately an opaque `uint16` bit container; the shared
ABI preserves all binary16 bit patterns but does not perform host-side numeric
conversion.

The triangle puts `float16_t2` values directly in its root and converts them to
`float2` in the shader. `clean_gfx` currently requires and enables:

- `storagePushConstant16` for inline 16-bit root members;
- `shaderFloat16` for FP16 operations and conversions;
- `shaderInt16` for 16-bit integer shader values; and
- `storageBuffer16BitAccess` for 16-bit values reached through BDA-backed
  storage.

A pointer to a 16-bit pointee does not by itself require
`StoragePushConstant16`; the push-constant member is only the 64-bit address.
Inline `float16_t` or `uint16_t` root members do require 16-bit push-constant
storage.

The runtime does not enable `uniformAndStorageBuffer16BitAccess`, because the
current API does not expose descriptor-backed uniform/storage buffers. It also
does not enable the Vulkan 8-bit storage or `shaderInt8` features, even though
the compatibility header provides 8-bit CPU/Slang aliases. Do not place 8-bit
members in root or BDA data until those Vulkan requirements are added.

- [Vulkan 16-bit storage](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_16bit_storage.html)
- [`VkPhysicalDevice16BitStorageFeatures`](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDevice16BitStorageFeatures.html)
- [Slang 16-bit push-constant capability test](https://github.com/shader-slang/slang/blob/master/tests/spirv/capability-storage-push-constant.slang)
- [Slang BDA-pointer capability regression test](https://github.com/shader-slang/slang/blob/master/tests/spirv/bda-pointer-no-spurious-storage-capability.slang)

## Summary of current boundaries

- Native descriptor-heap compilation requires Slang v2026.14.1+.
- The mapped shader path supports older Slang, but still requires
  `VK_EXT_descriptor_heap`; it is not a conventional descriptor-set backend.
- The scalar-layout fallback is verified only for the explicitly padded triangle
  structures.
- Resource-heap indices currently address image-sized slots; mixed descriptor
  types need a deliberate unified-stride or typed-index policy.
- Resource and sampler heap indices are separate namespaces.
- Push-data offset and size must be four-byte multiples and must fit the
  device-reported maximum.
- BDA pointers are unbounded and require valid lifetime and alignment.
- Shared structures must remain POD and have their C++ layout asserted.
- Direct 16-bit root/pointee data requires the Vulkan features listed above.
