# Slang shader contract

`clean_gfx` uses Slang as the source language for a Vulkan-only binding model:

- a draw or dispatch may copy one C-compatible CPU root structure through push data;
- buffers and larger structures are reached through 64-bit typed buffer device address
  (BDA) pointers stored in that root; and
- textures and samplers are selected by integer indices into the
  application-owned `VK_EXT_descriptor_heap` resource and sampler heaps.

There are no application-visible descriptor sets, descriptor pools, or pipeline
layouts in the shader compilation path described below.

## Supported Slang version

**Slang v2026.14.1 or newer is required.** The cube uses native descriptor-heap
syntax. The rootless triangle does not access a descriptor heap.

**SPIRV-Tools v2026.3 or newer is also required.** Every generated module is
validated during the build, including native `SPV_EXT_descriptor_heap` use.

Slang v2026.14 added direct `ResourceDescriptorHeap` and
`SamplerDescriptorHeap` input syntax. The initial v2026.14.0 release had a
descriptor-heap `ConstantBuffer<T>` lowering bug: it emitted a storage-buffer
descriptor instead of a uniform-buffer descriptor. That bug was fixed in
v2026.14.1. The build requests `spvDescriptorHeapEXT` directly and fails if the
installed compiler does not provide it.

- [Direct descriptor-heap input support](https://github.com/shader-slang/slang/pull/11798)
- [v2026.14 `ConstantBuffer` issue](https://github.com/shader-slang/slang/issues/12226)
- [v2026.14.1 fix](https://github.com/shader-slang/slang/pull/12256)
- [Slang releases](https://github.com/shader-slang/slang/releases)

## Exact compiler options

[`examples/triangle/CMakeLists.txt`](../examples/triangle/CMakeLists.txt) invokes
the following command for the vertex entry point:

```sh
slangc examples/triangle/triangle.slang \
  -target spirv \
  -profile spirv_1_6 \
  -emit-spirv-directly \
  -fvk-use-entrypoint-name \
  -entry vertexMain \
  -stage vertex \
  -o triangle.vert.spv
```

The fragment invocation changes the entry point, stage, and output to
`fragmentMain`, `fragment`, and `triangle.frag.spv`. The minimal triangle has no
compute stage and no descriptor-heap access, so its compiler command requests no
descriptor-heap capability. It declares no root block; positions and colors are
selected from shader constants using `SV_VertexID`.

[`examples/cube/CMakeLists.txt`](../examples/cube/CMakeLists.txt) adds the native
descriptor-heap options to otherwise equivalent vertex and fragment commands:

```sh
slangc examples/cube/cube.slang \
  -target spirv \
  -profile spirv_1_6 \
  -emit-spirv-directly \
  -fvk-use-entrypoint-name \
  -fvk-use-c-layout \
  -matrix-layout-row-major \
  -capability spvDescriptorHeapEXT \
  -I examples/cube \
  -I include \
  -entry fragmentMain \
  -stage fragment \
  -o cube.frag.spv
```

The cube vertex invocation changes the entry point, stage, and output to
`vertexMain`, `vertex`, and `cube.vert.spv`.

The significant options are:

- `-target spirv -profile spirv_1_6` targets the SPIR-V version supported by the
  Vulkan 1.4 baseline used by this repository.
- `-emit-spirv-directly` selects Slang's direct SPIR-V backend explicitly.
- `-fvk-use-entrypoint-name` preserves names such as `vertexMain`; pipeline
  creation uses those names rather than `main`.
- `-fvk-use-c-layout` makes the shader representation agree with the C++ POD
  representation described below.
- `-matrix-layout-row-major` selects the matrix ABI used by the C++ matrix PODs
  in the cube and other matrix-using shaders.
- `-capability spvDescriptorHeapEXT` enables the cube's native
  `SPV_EXT_descriptor_heap` lowering. In Slang's capability definitions this
  also brings in `SPV_KHR_untyped_pointers`.

Each current resource heap contains only image descriptors and uses
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

## Descriptor-heap syntax

The cube declares sampler slots once in the header shared by C++ and Slang:

```cpp
enum class CubeSampler : uint32
{
    wrap_linear,
    wrap_point,
    clamp_linear,
    clamp_point,
    count,
};
```

The fragment shader in
[`examples/cube/cube.slang`](../examples/cube/cube.slang) is:

```slang
Texture2D<float4> texture = ResourceDescriptorHeap[root.texture_index];
SamplerState sampler =
    SamplerDescriptorHeap[uint32(CubeSampler::clamp_point)];
```

The application chooses both kinds of index by choosing descriptor byte offsets. Resource and
sampler indices are separate namespaces even though both are represented as `uint32`. The cube
writes its texture into resource slot zero. Its shared C++/Slang `CubeSampler` enum assigns
`wrap_linear`, `wrap_point`, `clamp_linear`, and `clamp_point` to sampler slots 0 through 3, so no
runtime sampler index is needed in the root.

## Application-owned descriptor heaps

Descriptor heaps have explicit allocation calls because Vulkan gives descriptor-capable buffers
heap-specific size, alignment, and reserved-range rules:

```cpp
auto resource_heap = device.gpu_malloc_resource_heap(
    device.caps().image_descriptor_size);
auto sampler_heap = device.gpu_malloc_sampler_heap(
    device.caps().sampler_descriptor_size *
        static_cast<std::uint32_t>(CubeSampler::count));
```

Both allocations return directly usable coherent `cpu_ptr` and `gpu_ptr` values. Their reported
`size` is exactly the requested user byte count. Vulkan requires an implementation-reserved region;
clean_gfx appends it as a hidden suffix outside the returned range, so user descriptor slot zero is
exactly the returned pointer. Slot `i` begins at `cpu_ptr + i * descriptor_size`.
Each heap allocation owns one exact-sized descriptor-capable `VkBuffer` and one dedicated coherent
mapped device-memory allocation. Its private size includes alignment padding and the hidden suffix;
descriptor heaps are not suballocated from the ordinary 256 MiB buffer pages.

`Device::write_texture_descriptor()` writes a sampled or storage descriptor to a caller-selected
CPU address. `TextureViewDesc::mip_count == 0` selects every mip from `base_mip` onward.
`Device::write_sampler_descriptor()` writes a sampler descriptor without creating a public sampler
object. A command list that executes a shader using either heap binds the user ranges explicitly:

```cpp
commands.set_resource_heap({resource_heap.gpu_ptr, resource_heap.size});
commands.set_sampler_heap({sampler_heap.gpu_ptr, sampler_heap.size});
```

Heaps are command state, not device-global retained objects. The backend neither owns descriptor
slots nor discovers heap references from copied root data. The application must keep the heap
allocations, descriptor contents, textures named by descriptors, and all other referenced resources
alive and unchanged through GPU completion.

Slang can also materialize a resource-specific `.Handle` from a heap access for
storage or passing through user code. Internally descriptor handles use a
`uint2` representation. A combined texture/sampler handle uses `.x` for the
resource heap and `.y` for the sampler heap.

Under `SPV_EXT_descriptor_heap`, dynamically selected heap descriptors are
non-uniform by default. No conventional descriptor-set declarations or
`NonUniform` decoration are needed for legality.

See [Slang direct descriptor-heap indexing](https://github.com/shader-slang/slang/blob/master/docs/user-guide/03-convenience-features.md#direct-descriptor-heap-indexing)
and [the SPIR-V-specific lowering](https://github.com/shader-slang/slang/blob/master/docs/user-guide/03-convenience-features.md#spir-v-with-spvdescriptorheapext).

## Optional CPU root data and `vkCmdPushDataEXT`

Every draw and dispatch takes its optional root as the first argument. Pass `nullptr` when the
shader declares no root block; this records the Vulkan draw or dispatch without calling
`vkCmdPushDataEXT`:

```cpp
commands.draw(nullptr, 3);
```

Otherwise, the root is an ordinary CPU POD and the typed overload infers its complete size:

```cpp
RootArguments root{};
root.vertices = static_cast<Vertex*>(vertices.gpu_ptr);

commands.draw(&root, vertex_count);
// commands.dispatch(&root, x, y, z);
```

The typed methods infer `sizeof(RootArguments)` from the pointed-to type
and record `vkCmdPushDataEXT` immediately before the corresponding Vulkan draw or dispatch command.
Vulkan copies those CPU bytes while recording, so the root may be stack-local and may be changed or
destroyed as soon as the method returns. The root size must not exceed
`DeviceCaps::max_push_data_size`. There is no separate public push-data command and no partial-update
API.

The shader reads the same structure directly from the ordinary SPIR-V `PushConstant` storage class:

```slang
[[vk::push_constant]]
ConstantBuffer<RootArguments> root;
```

`vkCmdPushDataEXT` needs no new Slang storage kind. GPU pointers copied as fields of the root still
refer to GPU allocations, so those pointees must remain alive through execution even though the CPU
root itself has no post-call lifetime requirement. Do not mix this model with `vkCmdPushConstants`
or push descriptors because the Vulkan commands invalidate one another's push-data state.

- [Slang push constants](https://github.com/shader-slang/slang/blob/master/docs/user-guide/a2-01-spirv-target-specific.md#push-constants)
- [`VK_EXT_descriptor_heap` proposal, including push data](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_EXT_descriptor_heap.adoc)

## GPU pointers backed by buffer device addresses

Shared structures use an ordinary pointer type for a device pointer. The cube root is one example:

```c
struct CubeRootArguments
{
    CubeVertex* vertices;
    uint32 texture_index;
    uint32 padding;
    float4x4 mvp;
};
```

The triangle is rootless. The cube's sampler selection is the shared compile-time `CubeSampler`
enum rather than another root value. Both C++ and Slang see a typed `CubeVertex*`. `gpu_malloc()` returns a trivial
`GpuAllocation {cpu_ptr, gpu_ptr, size}` aggregate. The default `MemoryType::default_` and readback
allocations provide both addresses from coherent mapped device-local memory. The allocation holds the vertices; the root itself remains an
ordinary CPU value:

```cpp
auto vertices = device.gpu_malloc<CubeVertex>(vertex_count);
CubeRootArguments root{.vertices = static_cast<CubeVertex*>(vertices.gpu_ptr)};
commands.draw(&root, vertex_count);
```

The CPU pointer names the persistently mapped bytes and can be written directly.
The GPU pointer is only a carrier for GPU virtual-address bits on the CPU and
must not be dereferenced there. The shader can use normal typed access:

```slang
CubeVertex vertex = root.vertices[vertex_id];
```

Slang lowers such pointers to the SPIR-V `PhysicalStorageBuffer` storage class,
the `PhysicalStorageBufferAddresses` capability, and the
`PhysicalStorageBuffer64` addressing model. A more explicit shader-only spelling
is `Ptr<T, Access.Read, AddressSpace.Device>`, but the native `T*` spelling keeps
the shared header simple.

`gpu_malloc()` with `MemoryType::gpu_only` still returns the same POD, but `cpu_ptr`
is null because the allocation is not host-visible; `gpu_ptr` and `size` remain
valid. `gpu_free()` takes the complete, unchanged `GpuAllocation` returned by
`gpu_malloc()`, not one of its pointers or a manually constructed range.

If GPU memory cannot satisfy an allocation, `gpu_malloc()` and the two descriptor-heap allocation
functions return `GpuAllocation {nullptr, nullptr, 0}`. A successful `MemoryType::gpu_only` allocation
also has a null `cpu_ptr`, so `gpu_ptr` is the validity field. The examples keep
their paths direct and deliberately do not test their small allocations for failure.

Address-based command APIs use a second trivial aggregate,
`GpuRange {gpu_ptr, size}`. Construct it from the allocation's GPU fields for a
whole allocation, or supply an interior GPU pointer and the exact byte count for
that subrange. The backend passes this address/size pair directly to the Vulkan
device-address command. Command recording performs no allocation lookup and does not retain the
allocation; validity, bounds, and lifetime are the application's responsibility.

Internally the Vulkan runtime enables `bufferDeviceAddress`, backs allocations
with 256 MiB heap pages, and gives each page one fully bound universal buffer
created for shader-address, storage, index, indirect, and transfer use. It obtains
the page's base address with `vkGetBufferDeviceAddress` and suballocates it with the pinned
[OffsetAllocator](https://github.com/sebbbi/OffsetAllocator) revision. Additional
pages are created on exhaustion. `MemoryType::default_` is not a host-only staging class: device
creation rejects hardware without coherent host-visible memory on a device-local heap. Descriptor
heaps use their dedicated buffers described above. No Vulkan buffer handle or owning allocation
class is visible to the application; `GpuAllocation` and `GpuRange` are only POD address/size
carriers.

Important pointer limitations are:

- a BDA pointer has no bound, so robust buffer access cannot make an out-of-range
  dereference safe;
- the CPU representation carries a GPU address and cannot be dereferenced by host code;
- the application must keep every GPU allocation referenced through the root alive through GPU
  completion and satisfy the pointee's alignment; the CPU root itself is copied during recording;
- opaque textures cannot be pointees—write their descriptors into a resource heap;
- sampler state is written directly into a sampler heap rather than represented by a public
  sampler object;
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

Shader and CPU code include the same application structure header when they share data, as the cube
does with [`examples/cube/cube_shared.h`](../examples/cube/cube_shared.h). It first includes
`<clean_gfx/shader_types.h>`. The rootless triangle needs no shared structure header.

Slang predefines `__SLANG__`, which `shader_types.h` uses to select its shader branch. That branch
uses Slang's native `float2`, `float3`, `float4`, `int2`, `uint4`, `float16_t2`, `int16_t3`,
`uint16_t4`, matrix types, and similar types. On the C++ branch it declares scalar-member POD types
with the same names and verifies their size, alignment, member offsets, triviality, IEEE-754 float
representation, and little-endian host byte order.

`-fvk-use-c-layout` is required because it applies C/C++ layout rules to values
accessed through `ConstantBuffer`, `ParameterBlock`, `StructuredBuffer`,
`ByteAddressBuffer`, and general pointers. Every shader sharing C++ POD structures is compiled
with this option. Structures containing matrices also require
`-matrix-layout-row-major`. The provided
`float2x2`, `float3x3`, `float4x4`, and `float3x4` C++ POD names match Slang's
built-ins and store compact rows. Slang names matrix dimensions as rows by columns,
so `float3x4` contains three `float4` rows. Shared structures do not
support an alternate layout mode.

The runtime requires and enables Vulkan `scalarBlockLayout`. Keep shared structures simple:

- use the scalar-member types supplied by `shader_types.h`, not host SIMD types
  with platform-specific alignment;
- make padding explicit when the ABI is externally visible;
- use the supplied matrix PODs with the row-major compiler option; the
  cube uses `float4x4` and `mul(matrix, vector)`; and
- reserve ordinary pointer fields for GPU addresses, and keep host pointers,
  references, containers, constructors, and virtual
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

Neither current example places a 16-bit value in its root or vertex data.
`clean_gfx` nevertheless currently requires and enables:

- `shaderFloat16` for FP16 operations and conversions;
- `shaderInt16` for 16-bit integer shader values;
- `storagePushConstant16` for 16-bit values copied directly into a root; and
- `storageBuffer16BitAccess` for 16-bit values reached through BDA-backed
  storage.

The root itself uses push-constant storage, while a BDA pointee uses storage-buffer access. Both
locations therefore support 16-bit values through their corresponding enabled feature.

The runtime does not enable `uniformAndStorageBuffer16BitAccess`, because the
current API does not expose descriptor-backed uniform/storage buffers. It also
does not enable the Vulkan 8-bit storage or `shaderInt8` features, even though
the shared type header provides 8-bit CPU/Slang aliases. Do not place 8-bit
members in root or BDA data until those Vulkan requirements are added.

- [Vulkan 16-bit storage](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_16bit_storage.html)
- [`VkPhysicalDevice16BitStorageFeatures`](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDevice16BitStorageFeatures.html)
- [Slang BDA-pointer capability regression test](https://github.com/shader-slang/slang/blob/master/tests/spirv/bda-pointer-no-spurious-storage-capability.slang)

## Summary of current boundaries

- Slang v2026.14.1+ and native `SPV_EXT_descriptor_heap` compilation are required.
- Every shared shader/CPU structure uses `-fvk-use-c-layout`; no alternate layout mode is supported.
- Resource-heap indices currently address image-sized slots; mixed descriptor
  types need a deliberate unified-stride or typed-index policy.
- Resource and sampler heap indices are separate namespaces.
- Resource and sampler heaps are application-owned `GpuAllocation` values, must be bound explicitly,
  and must remain alive through GPU completion.
- Every draw and dispatch takes its optional root first. `nullptr` emits no push data; a typed CPU
  root pointer copies `sizeof(Root)` bytes through `vkCmdPushDataEXT` while recording.
- BDA pointers are unbounded and require valid lifetime and alignment.
- Shared structures must use compatible plain C layout.
- Direct 16-bit root fields and 16-bit BDA pointee data use the corresponding Vulkan features listed
  above.
- The runtime is single-threaded and performs no mutex or atomic synchronization; do not record or
  submit through one device concurrently.
