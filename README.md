# clean_gfx

`clean_gfx` is a small Vulkan-only rendering layer built around the binding model from
[No Graphics API](https://www.sebastianaaltonen.com/blog/no-graphics-api): push one CPU POD root,
follow typed buffer-device-address pointers from that root, and use integer indices for the one
global texture heap and sampler heap.

The implementation deliberately creates no `VkDescriptorSetLayout`, `VkDescriptorPool`,
`VkDescriptorSet`, or `VkPipelineLayout`. Internally it uses:

- `VK_EXT_descriptor_heap` for the resource/sampler heaps and `vkCmdPushDataEXT`;
- `VK_KHR_device_address_commands` for index binding, indirect draws/dispatch, and buffer/image copies;
- Slang pointers for custom vertex fetch and arbitrary BDA-backed structures;
- Vulkan 1.4 dynamic rendering and synchronization2 for the remaining fixed-function work.

The backend retains only the Vulkan ownership Vulkan still requires. Shader-visible buffers
are `uint64` device addresses, sampled/storage textures are 32-bit resource-heap indices, and
samplers are 32-bit sampler-heap indices. An upload buffer exposes both `mapped_data()` and
`address()`, directly covering the proposal's host-pointer-to-GPU-pointer use case without a public
Vulkan buffer handle.

## Requirements

- A 64-bit, little-endian host, a Vulkan 1.4 loader/device, and the loader's development library.
- `VK_EXT_descriptor_heap`, `VK_KHR_device_address_commands`, and
  `VK_KHR_shader_untyped_pointers`.
- The BDA, descriptor-heap, device-address-command, untyped-pointer, dynamic-rendering,
  synchronization2, scalar-layout, and 16-bit features checked at startup.
- CMake 3.24+, a C++20 compiler, and `slangc`.
- Slang 2026.14.1+ is recommended for direct `SPV_EXT_descriptor_heap` emission. Older Slang
  automatically uses the extension's set/binding mapping interface without creating descriptor
  sets or layouts.

The build fetches Vulkan-Headers 1.4.357 because older installed SDK headers do not declare these
extensions. Set `CLEAN_GFX_FETCH_VULKAN_HEADERS=OFF` to require a sufficiently new system SDK.

## Build and run

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/examples/triangle/clean_gfx_triangle triangle.ppm
```

The sample is intentionally offscreen, so WSI does not obscure the binding model. A compute shader
first writes a 2x2 texture through its storage-heap index. Graphics then pushes a 32-byte root
containing a typed vertex BDA, sampled-texture/sampler indices, FP32, and FP16 values, binds index
data by GPU address, renders a triangle, and copies the result to a PPM through another
device-address command.

The sample will not run through MoltenVK. On this development Mac it is expected to stop during
Vulkan initialization; run it on a driver that exposes all required extensions.

## Shared root ABI

The same header is included by C++ and Slang:

```cpp
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
```

`CLEAN_GFX_DEVICE_PTR(T)` becomes an opaque `uint64` on C++ and a typed `T*` in Slang. This keeps the
bytes identical without constructing a host pointer that could accidentally be dereferenced.
`shader_types.h` supplies scalar-layout POD equivalents for Slang vector types, and the shader is
compiled with `-fvk-use-c-layout`. Both sides assert the example's member offsets and 32-byte size.

The command-side call is just:

```cpp
commands.push_root(root); // vkCmdPushDataEXT; no layout or stage mask
commands.draw_indexed(index_buffer.slice(), clean_gfx::IndexType::uint32, 3);
```

The native Slang fragment path indexes the heaps directly:

```slang
Texture2D<float4> texture = ResourceDescriptorHeap[root.texture_index];
SamplerState sampler = SamplerDescriptorHeap[root.sampler_index];
```

## Scope

Implemented now: capability-driven device creation, mapped/GPU/readback BDA buffers, one persistent
resource heap and sampler heap, sampled and storage image descriptors, samplers, graphics and compute
pipelines with null layouts, CPU root pushing, dynamic rendering, direct/indexed/indirect draws,
direct/indirect compute, device-address copies, global memory barriers, image transitions, and
synchronous submission.

This is a focused prototype, not a production allocator. Buffer and image allocations are dedicated,
submission is synchronous, the sample is offscreen, and ray tracing, mesh shading, sparse heaps,
capture/replay, device-generated commands, swapchains, multi-queue scheduling, and pipeline caching are
out of scope. Vulkan images and pipelines remain real objects, and image-layout transitions are still
needed; the three requested extensions do not eliminate those parts of Vulkan.

Texture upload/readback helpers currently address mip level zero; additional mip levels can be
created and transitioned, but populating them requires extending the copy API.

`BufferSlice` values carry hidden allocation/device metadata and keep command-address memory alive;
raw `DeviceAddress` values remain available for shader roots. Because pushed root bytes are opaque,
resources referenced only by a root pointer or heap index must remain alive until the synchronous
submission returns. Image transitions intentionally take an explicit `before` state; the caller must
name the image's actual current state.

See [Vulkan support and cross-reference](docs/vulkan-support.md) and
[Slang integration and ABI](docs/slang.md) for the exact contracts and current driver notes.
