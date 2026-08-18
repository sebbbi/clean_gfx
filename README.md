# clean_gfx

`clean_gfx` is a small Vulkan-only rendering layer built around the binding model from
[No Graphics API](https://www.sebastianaaltonen.com/blog/no-graphics-api): optionally copy one small
CPU root structure into a draw or dispatch, follow 64-bit GPU pointers stored in that root, and use
integer indices into application-owned resource and sampler heaps. Its C++ API lives in the short
`gfx` namespace.

The implementation deliberately creates no `VkDescriptorSetLayout`, `VkDescriptorPool`,
`VkDescriptorSet`, or `VkPipelineLayout`. Internally it uses:

- `VK_EXT_descriptor_heap` for the resource/sampler heaps and `vkCmdPushDataEXT`;
- `VK_KHR_device_address_commands` for index binding, indirect draws/dispatch, and buffer/image copies;
- `VK_KHR_unified_image_layouts` to keep every normal image access in `VK_IMAGE_LAYOUT_GENERAL`;
- Slang pointers for custom vertex fetch and arbitrary BDA-backed structures;
- Vulkan 1.4 dynamic rendering and synchronization2 for the remaining fixed-function work.

There is no public buffer object or owning memory class. `gpu_malloc<T>()` returns the plain
`GpuAllocation<T> {cpu, gpu, size}` aggregate. Its default `MemoryType::default_` is strictly
device-local, host-visible, and host-coherent, so ordinary GPU allocations expose both a
persistently mapped CPU pointer and their GPU pointer. `MemoryType::readback` uses the same required
memory properties and additionally prefers host-cached memory. `MemoryType::gpu_only` is
non-host-visible, so its `cpu` pointer is null. `gpu_free()` takes the unchanged allocation by
reference. Address-based command APIs take the separate `GpuRange {gpu, size}` aggregate by
reference, and `gpu_range()` converts
an allocation to its full range. This lets a caller bind all or part of an allocation
without exposing a Vulkan buffer handle. `gpu_malloc_resource_heap()` and
`gpu_malloc_sampler_heap()` create coherent, directly writable descriptor heaps. The application
chooses 32-bit resource/sampler slot indices, writes descriptors through the returned CPU pointer,
and explicitly binds the corresponding GPU range on command lists that use them.

The public API is a set of free functions in namespace `gfx`. Devices, textures, pipelines, and
command lists are opaque raw pointer handles rather than C++ ownership wrappers. After device
initialization, creation returns a handle directly and matching `destroy_*()` functions release it;
applications destroy owned handles explicitly in reverse creation order. A submitted command list
is consumed by `submit()` or `submit_and_wait()`; `destroy_command_list()` abandons one that was not
submitted. `get_device_caps()` returns a reference into the device and does not copy the capability
record; that reference remains valid until `destroy_device()`.

CPU arrays in public descriptors use `Span<T> {data, size}`, a non-owning pointer/count view with no
iterator or accessor layer. It constructs from a pointer and count or a C array. `Span<const T>` also
accepts an initializer list for concise call arguments. Initializer-list storage remains alive only
through that call, so a span backed by it must never be retained.

Every public descriptor field has a useful default. Call sites use C++20 designated initializers,
name each explicitly supplied field, and omit fields whose defaults are already correct. `Span` is
the constructor-bearing exception because its three concise input forms are part of the API.

The library is built without C++ exception handling. Programming errors and Vulkan failures after
device creation assert and abort. `create_device()` is the one recoverable object-creation operation:
it returns `DeviceInit {device, error}` because no device exists yet on which to report a fatal
runtime failure. CPU allocation failure is deliberately not handled. GPU heap exhaustion is the one
allocation-specific case: `gpu_malloc()`, `gpu_malloc_resource_heap()`, and
`gpu_malloc_sampler_heap()` return the default null `GpuAllocation {}`. Test
`gpu` if an application wants to recover; the examples deliberately assume their tiny
allocations succeed and perform no such checks.

## Requirements

- A 64-bit, little-endian host, a Vulkan 1.4 loader/device, and the loader's development library.
- `VK_EXT_descriptor_heap`, `VK_KHR_device_address_commands`,
  `VK_KHR_shader_untyped_pointers`, and `VK_KHR_unified_image_layouts`.
- The BDA, descriptor-heap, device-address-command, untyped-pointer, unified-image-layout,
  dynamic-rendering, synchronization2, scalar-layout, and 16-bit features checked at startup.
- A device-local memory type on an at least 256 MiB heap that is both host-visible and host-coherent.
  This is the default allocation class and is also used for readback and application-owned
  descriptor heaps. Host-only and non-coherent fallback memory are unsupported.
- A separate device-local, non-host-visible memory type on an at least 256 MiB heap for GPU-only
  allocations and textures.
- CMake 3.24+, a C++20 compiler, Slang 2026.14.1 or newer, SPIRV-Tools 2026.3 or
  newer, and a system Vulkan SDK whose headers report version 1.4.357 or newer. The cube requires
  Slang's native `SPV_EXT_descriptor_heap` capability. Shaders sharing C++ POD structures use
  `-fvk-use-c-layout`; matrix-bearing structures additionally use `-matrix-layout-row-major`.

The build uses the Vulkan headers and loader development library supplied by that system SDK and
rejects versions below 1.4.357 during configuration.

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

The 512x512 triangle follows the first triangle in the
[Khronos Vulkan Tutorial](https://docs.vulkan.org/tutorial/latest/03_Drawing_a_triangle/02_Graphics_pipeline_basics/01_Shader_modules.html):
the vertex shader generates three positions and colors from `SV_VertexID`, and the CPU records one
rootless `draw(nullptr, 3)`. It has no vertex allocation, texture, descriptor heap, compute stage, or
depth attachment.

The 500x500 cube follows the official
[Khronos/LunarG `vkcube` sample](https://github.com/KhronosGroup/Vulkan-Tools/tree/vulkan-sdk-1.4.357.0/cube).
It spins four degrees per frame and reproduces the sample's cube geometry, camera, nearest-filtered
sRGB texture, face lighting, clear color, and depth test. Vertex data is fetched through BDA, while
the application allocates, writes, and binds one resource heap and one sampler heap. The embedded
texture is the exact upstream
[`lunarg.ppm.h` asset](https://github.com/KhronosGroup/Vulkan-Tools/blob/vulkan-sdk-1.4.357.0/cube/lunarg.ppm.h),
distributed with the sample under the upstream
[Apache-2.0 license](https://github.com/KhronosGroup/Vulkan-Tools/blob/vulkan-sdk-1.4.357.0/LICENSE.txt).

The examples will not run through MoltenVK. On this development Mac they are expected to stop during
Vulkan initialization; run them on a driver that exposes all required extensions.

## Optional shared root ABI

When a shader needs root data, the same structure header is included by C++ and Slang:

```cpp
struct RootArguments
{
    Vertex* vertices;
};
```

The root is an ordinary CPU POD. Its pointer fields carry GPU addresses and must not be dereferenced
by the CPU. `shader_types.h` supplies C-layout-compatible POD equivalents for Slang vector types
and `float3x4`. Shared structures use `-fvk-use-c-layout`, plus `-matrix-layout-row-major` when they
contain the matrix. Slang names matrix dimensions as row count by column count, so the C++
`float3x4` stores three `float4` rows. The draw/dispatch template copies the complete root through
`vkCmdPushDataEXT` while the command is recorded, so the CPU root need not outlive the call.

The first argument is `nullptr` when a draw or dispatch has no root. No push-data command is emitted
in that case:

```cpp
gfx::draw(commands, nullptr, 3);

RootArguments root{.vertices = vertices.gpu};
gfx::draw(commands, &root, vertex_count);
```

The triangle is deliberately rootless. The cube uses a root containing a vertex pointer, texture
index, a shared `float3x4` clip transform, and two depth coefficients. Sampler slots use one enum
shared by C++ and Slang:

```slang
Texture2D<float4> texture = ResourceDescriptorHeap[root.texture_index];
SamplerState sampler =
    SamplerDescriptorHeap[uint32(CubeSampler::clamp_point)];
```

`CubeSampler` assigns `wrap_linear`, `wrap_point`, `clamp_linear`, and `clamp_point` to slots 0
through 3. The application writes all four sampler descriptors into those exact slots.

## Scope

Implemented now: capability-driven device creation, plain GPU allocation and range aggregates, coherent
device-local mapped default/readback memory, GPU-only memory, application-owned resource/sampler heap
allocations, sampled and storage image descriptor writers, sampler descriptor writers, explicit heap
binding, graphics and compute pipelines with null layouts, optional CPU-root draw/dispatch calls, dynamic rendering,
direct/indexed/indirect draws, direct/indirect compute, device-address copies, global memory
barriers, unified image layouts, and timeline-backed asynchronous submission.

Ordinary value memory is suballocated from 256 MiB internal pages, with one fully bound universal
`VkBuffer` per page and separate pools for default, GPU-only, and readback memory. Suballocation uses
a pinned revision of [OffsetAllocator](https://github.com/sebbbi/OffsetAllocator), and a new page is
created when existing pages cannot satisfy a request.

Every resource or sampler heap instead owns one exact-sized descriptor-capable `VkBuffer` and one
dedicated allocation from the same coherent mapped device-local memory used by default allocations.
The buffer covers the aligned user bytes, suffix padding, and Vulkan-required implementation
reservation; no 256 MiB descriptor page is created. The public `GpuAllocation` exposes only the
requested user range. Slot zero is therefore exactly
`cpu`/`gpu`; slot `i` is `i * DeviceCaps::image_descriptor_size` bytes into a resource heap
or `i * DeviceCaps::sampler_descriptor_size` bytes into a sampler heap.

Textures are normally suballocated from separate 256 MiB GPU-only image-memory heaps; only images
that Vulkan requires to be dedicated or that cannot fit a page use the dedicated-allocation path.
`create_texture()` does not submit work: the next `begin_commands()` batches the one required
`VK_IMAGE_LAYOUT_UNDEFINED` to `VK_IMAGE_LAYOUT_GENERAL` metadata initialization for every newly
created texture. Sampled/storage
descriptors are generated directly from image create information; a real `VkImageView` is created
only when fixed-function attachment use requires one.

Two persistent frame contexts own reusable command pools, command buffers, and timeline values.
`submit()` queues work and returns without waiting; `submit_and_wait()` is the synchronous
convenience path. Both `submit_and_wait()` and `wait_idle()` reset completed command pools after
waiting, releasing descriptor-heap reserved ranges as well as retiring execution. The backend
retains no allocations, textures, pipelines, descriptor slots, or other user resources for a
command list. The caller must keep every referenced object/allocation alive and must not call
`gpu_free()` or reuse descriptor storage until one of those completion paths has retired the work.

This remains a focused, deliberately single-threaded prototype. Device and command-list operations
are not thread-safe; the implementation contains no mutex or atomic synchronization. Ray tracing,
mesh shading, sparse heaps, capture/replay, device-generated commands, Vulkan WSI and swapchains,
multi-queue scheduling, and pipeline caching are out of scope. The Win32/GDI windows belong only to
the examples and present CPU-readback pixels. Vulkan images and pipelines remain real objects, but
normal image accesses remain in `VK_IMAGE_LAYOUT_GENERAL` for the complete image lifetime.

Texture copy commands currently address mip level zero; additional mip levels can be created, but
populating them requires extending the copy API. The unified-layout extension deliberately does not
remove the one-time initialization out of `VK_IMAGE_LAYOUT_UNDEFINED`; clean_gfx records it lazily
in the next command-list's batched texture-initialization dependency.

Address-based command functions consume `GpuRange` aggregates directly. Command recording performs no
hidden allocation lookup or command-list lifetime retention. GPU pointers copied from root data and
descriptor-heap contents are opaque to the backend, so all pointed-to allocations, textures,
pipelines, heaps, and descriptor slots remain the caller's lifetime responsibility through GPU completion. Callers
synchronize genuine read/write hazards with `barrier()`; no image-layout state is
exposed by the public API.

See [Vulkan support and cross-reference](docs/vulkan-support.md) and
[Slang integration and ABI](docs/slang.md) for the exact contracts and current driver notes.
