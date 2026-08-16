# clean_gfx

`clean_gfx` is a small Vulkan-only rendering layer built around the binding model from
[No Graphics API](https://www.sebastianaaltonen.com/blog/no-graphics-api): push one CPU POD root,
follow typed buffer-device-address pointers from that root, and use integer indices for the one
global texture heap and sampler heap. Its C++ API lives in the short `gfx` namespace.

The implementation deliberately creates no `VkDescriptorSetLayout`, `VkDescriptorPool`,
`VkDescriptorSet`, or `VkPipelineLayout`. Internally it uses:

- `VK_EXT_descriptor_heap` for the resource/sampler heaps and `vkCmdPushDataEXT`;
- `VK_KHR_device_address_commands` for index binding, indirect draws/dispatch, and buffer/image copies;
- `VK_KHR_unified_image_layouts` to keep every normal image access in `VK_IMAGE_LAYOUT_GENERAL`;
- Slang pointers for custom vertex fetch and arbitrary BDA-backed structures;
- Vulkan 1.4 dynamic rendering and synchronization2 for the remaining fixed-function work.

There is no public buffer object or owning memory class. `Device::gpu_malloc()` returns the plain
`GpuAllocation {cpu_ptr, gpu_ptr, size}` POD. Upload and readback allocations expose both their
persistently mapped CPU pointer and their GPU pointer; `MemoryType::gpu` is non-host-visible, so its
`cpu_ptr` is null. `gpu_free()` takes the unchanged allocation POD. Address-based command APIs take
the separate `GpuRange {gpu_ptr, size}` POD, allowing a caller to bind all or part of an allocation
without exposing a Vulkan buffer handle. Sampled/storage textures are 32-bit resource-heap indices,
and samplers are 32-bit sampler-heap indices.

The library is built without C++ exception handling. Programming errors are assertions. Operations
that can fail for runtime reasons return `Error`; object factories take an output object first and
leave it empty on failure. CPU allocation failure is deliberately not handled. GPU heap exhaustion
is the one allocation-specific case: `gpu_malloc()` returns the canonical null
`GpuAllocation {nullptr, nullptr, 0}`. Test `gpu_ptr` if an application wants to recover; the
examples deliberately assume their tiny allocations succeed and perform no such checks.

## Requirements

- A 64-bit, little-endian host, a Vulkan 1.4 loader/device, and the loader's development library.
- `VK_EXT_descriptor_heap`, `VK_KHR_device_address_commands`,
  `VK_KHR_shader_untyped_pointers`, and `VK_KHR_unified_image_layouts`.
- The BDA, descriptor-heap, device-address-command, untyped-pointer, unified-image-layout,
  dynamic-rendering, synchronization2, scalar-layout, and 16-bit features checked at startup.
- A device-local memory type on an at least 256 MiB heap that is both host-visible and host-coherent.
  All mapped allocations and descriptor heaps use this memory directly; non-coherent fallback
  memory is unsupported.
- A separate device-local, non-host-visible memory type on an at least 256 MiB heap for GPU-only
  allocations and textures.
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
./build/examples/triangle/clean_gfx_triangle
./build/examples/cube/clean_gfx_cube
```

On Windows, either executable opens a visible Win32 window when run without arguments. The examples
render into Vulkan images, copy completed pixels to coherent readback memory, and display them with
an example-only GDI CPU blit. The core `gfx` library remains WSI-free: it creates no Vulkan surface
or swapchain and has no presentation API.

Passing a path selects deterministic one-frame PPM output instead of the window:

```sh
./build/examples/triangle/clean_gfx_triangle triangle.ppm
./build/examples/cube/clean_gfx_cube cube.ppm
```

The 512x512 triangle is the minimal RGB
[Khronos Hello Triangle 1.3](https://github.com/KhronosGroup/Vulkan-Samples/tree/main/samples/api/hello_triangle_1_3):
three colored vertices fetched through one BDA pointer, one untextured non-indexed draw, and the
Khronos sample's dark-blue clear color. It has no compute stage and does not index a descriptor heap.

The 500x500 cube follows the official
[Khronos/LunarG `vkcube` sample](https://github.com/KhronosGroup/Vulkan-Tools/tree/vulkan-sdk-1.4.357.0/cube).
It spins four degrees per frame and reproduces the sample's cube geometry, camera, nearest-filtered
sRGB texture, face lighting, clear color, and depth test. Vertex data is fetched through BDA, while
the texture and sampler use the resource and sampler descriptor heaps. The embedded texture is the
exact upstream
[`lunarg.ppm.h` asset](https://github.com/KhronosGroup/Vulkan-Tools/blob/vulkan-sdk-1.4.357.0/cube/lunarg.ppm.h),
distributed with the sample under the upstream
[Apache-2.0 license](https://github.com/KhronosGroup/Vulkan-Tools/blob/vulkan-sdk-1.4.357.0/LICENSE.txt).

The examples will not run through MoltenVK. On this development Mac they are expected to stop during
Vulkan initialization; run them on a driver that exposes all required extensions.

## Shared root ABI

The same header is included by C++ and Slang:

```cpp
struct RootArguments
{
    Vertex* vertices;
};
```

The field is an ordinary typed pointer on both C++ and Slang. C++ initializes it by casting the
allocation POD's `gpu_ptr` to the shared pointer type; that value is only an ABI carrier and must not
be dereferenced by the CPU. `shader_types.h` supplies scalar-layout POD equivalents for Slang vector
types, and the shader is compiled with `-fvk-use-c-layout`. Both sides assert the vertex layout and
the root's single-pointer, 8-byte ABI.

The command-side call is just:

```cpp
commands.push_root(root); // vkCmdPushDataEXT; no layout or stage mask
commands.draw(3);
```

The triangle is deliberately pointer-only. The cube extends its root with texture and sampler
indices plus an explicitly represented transform, and its native Slang fragment path indexes the
heaps directly:

```slang
Texture2D<float4> texture = ResourceDescriptorHeap[root.texture_index];
SamplerState sampler = SamplerDescriptorHeap[root.sampler_index];
```

## Scope

Implemented now: capability-driven device creation, POD GPU allocations and ranges, coherent
device-local mapped upload/readback memory, GPU-only memory, one persistent resource heap and sampler
heap, sampled and storage image descriptors, samplers, graphics and compute pipelines with null
layouts, CPU root pushing, dynamic rendering, direct/indexed/indirect draws, direct/indirect compute,
device-address copies, global memory barriers, unified image layouts, and synchronous submission.

GPU memory is suballocated from 256 MiB internal pages, with one fully bound universal `VkBuffer`
per page and separate page pools for upload, GPU-only, and readback memory. Suballocation uses a
pinned revision of [OffsetAllocator](https://github.com/sebbbi/OffsetAllocator), and a new page is
created when existing pages cannot satisfy a request. `gpu_free()` removes the allocation from the
live registry immediately, while any command list that already consumed one of its `GpuRange`s
retains the hidden record and delays reuse of the suballocation until that list is destroyed after
submission.

This remains a focused prototype: image allocations are dedicated, submission is synchronous, and
ray tracing, mesh shading, sparse heaps, capture/replay, device-generated commands, Vulkan WSI and
swapchains, multi-queue scheduling, and pipeline caching are out of scope. The Win32/GDI windows
belong only to the examples and present CPU-readback pixels. Vulkan images and pipelines remain real
objects, but normal image accesses remain in `VK_IMAGE_LAYOUT_GENERAL` for the complete image
lifetime.

Texture copy commands currently address mip level zero; additional mip levels can be created, but
populating them requires extending the copy API. Texture creation performs the sole
required `VK_IMAGE_LAYOUT_UNDEFINED` to `VK_IMAGE_LAYOUT_GENERAL` metadata initialization internally;
the unified-layout extension deliberately does not remove that one-time Vulkan requirement.

Address-based command methods accept `GpuRange` PODs. A hidden allocation registry resolves the GPU
pointer, asserts that the explicit byte range is valid, and retains its suballocation through
submission.
Pushed root bytes are opaque, so allocations referenced only by a root pointer—and resources
referenced only by a heap index—remain the caller's lifetime responsibility until
`submit_and_wait()` returns. Callers synchronize genuine read/write hazards with
`CommandList::barrier()`; no image-layout state is exposed by the public API.

See [Vulkan support and cross-reference](docs/vulkan-support.md) and
[Slang integration and ABI](docs/slang.md) for the exact contracts and current driver notes.
