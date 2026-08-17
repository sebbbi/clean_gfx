# Vulkan support and design mapping

Status date: 2026-08-16.

`clean_gfx` is a Vulkan-only implementation of the design proposed in
[No Graphics API](https://www.sebastianaaltonen.com/blog/no-graphics-api): an optional CPU root-data
block copied into a draw or dispatch, typed buffer device-address pointers stored in that block, and
32-bit indices into application-owned resource and sampler heaps.
There are no descriptor sets, descriptor pools, descriptor-set layouts, or pipeline layouts in the
public API or Vulkan backend.

MoltenVK is intentionally unsupported. The current capability check targets desktop Vulkan 1.4
drivers exposing all requirements below; a MoltenVK device that does not expose them is rejected.

`VK_EXT_descriptor_heap` (device extension 136, revision 1),
`VK_KHR_device_address_commands` (device extension 319, revision 1), and
`VK_KHR_unified_image_layouts` (device extension 528, revision 1) are ratified. `vkCmdPushDataEXT`
belongs to `VK_EXT_descriptor_heap`; it is not a separate extension.

## Runtime requirements

The extension specifications permit some Vulkan 1.2/1.3 configurations, but this repository
deliberately requires a Vulkan 1.4 loader and physical device. Configuration requires a system
Vulkan SDK whose headers report version 1.4.357 or newer.

| Requirement | Enabled feature/API | Why `clean_gfx` requires it |
|---|---|---|
| Vulkan 1.4 | `maintenance5` | Provides the 64-bit pipeline-create flags path used for descriptor-heap pipelines. |
| [`VK_EXT_descriptor_heap`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_descriptor_heap.html) | `VkPhysicalDeviceDescriptorHeapFeaturesEXT::descriptorHeap` | Application-owned resource/sampler heaps, host descriptor generation, explicit heap binding, `vkCmdPushDataEXT`, and pipelines with no layout. Capture/replay is not enabled. |
| [`VK_KHR_shader_untyped_pointers`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_untyped_pointers.html) | `shaderUntypedPointers` | Native `ResourceDescriptorHeap`/`SamplerDescriptorHeap` access. Slang BDA pointers use the separate physical-storage-buffer path. |
| [`VK_KHR_device_address_commands`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_device_address_commands.html) | `deviceAddressCommands` | Address-range index binding, indirect draw/dispatch, and buffer/image copies. |
| [`VK_KHR_unified_image_layouts`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_unified_image_layouts.html) | `unifiedImageLayouts` | Guarantees that `VK_IMAGE_LAYOUT_GENERAL` is efficient for every normal image use, eliminating public layout-state tracking. Video layouts are out of scope, so `unifiedImageLayoutsVideo` is not enabled. |
| [Buffer device address](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_buffer_device_address.html) | `bufferDeviceAddress` | `GpuAllocation`/`GpuRange` addresses, internal heap-page addresses, and typed pointers in shared root structures. |
| Vulkan 1.3 | `synchronization2`, `dynamicRendering` | Barriers and rendering without render-pass/framebuffer objects. |
| Vulkan 1.2 | `shaderFloat16`, `scalarBlockLayout` | Half-precision shared values and C-compatible shared layouts. |
| Vulkan 1.1/core | `storagePushConstant16`, `storageBuffer16BitAccess`, `shaderDrawParameters`, `shaderInt16`, graphics-stage stores/atomics, and storage-image reads/writes without a shader format | 16-bit members in copied root data and pointed-to storage, Slang's `SV_VertexID` lowering, and general sampled/storage texture use from graphics and compute shaders. |
| Host/queue | 64-bit little-endian host, IEEE-754 binary32, graphics-and-compute-capable queue | Shared pointer representation and the current single-queue command implementation. |
| Mapped device memory | A memory type and at least 256 MiB device-local heap with `DEVICE_LOCAL`, `HOST_VISIBLE`, and `HOST_COHERENT` | This strict GPU-memory class is `MemoryType::default_` and backs ordinary default allocations, readback, and descriptor heaps. It is persistently mapped without flush/invalidate operations; host-only and non-coherent fallbacks are unsupported. `HOST_CACHED` is preferred for readback but is not required. |
| GPU-only device memory | A memory type on an at least 256 MiB heap with `DEVICE_LOCAL` and without `HOST_VISIBLE` | `MemoryType::gpu_only` allocations and textures cannot fall back to CPU-visible memory. |

`VK_EXT_debug_utils` and `VK_LAYER_KHRONOS_validation` are optional. A library build without
`NDEBUG` enables them automatically when present; a Release build does not request them. There is
no runtime validation option in `DeviceDesc`.

## Error model

`clean_gfx` has no C++ exception path and all repository targets are compiled with exception
handling disabled. The public runtime result is deliberately small:

```cpp
enum class Error : std::uint8_t
{
    none,
    unsupported,
    out_of_device_memory,
    invalid_shader,
    device_lost,
    driver_error,
};
```

Fallible object factories take their output object first and return `Error`; the output remains
empty on failure. `CommandList::finish()`, `Device::submit()`, `Device::submit_and_wait()`, and
`Device::wait_idle()` also return `Error`. Incorrect handles, ranges, alignment, state transitions,
and other programming
errors are assertions rather than recoverable runtime results. CPU allocation failure is not
handled. GPU heap exhaustion is reported directly by every GPU allocation entry point as
`GpuAllocation {nullptr, nullptr, 0}`; a caller that wants to recover tests `gpu_ptr`.

The repository queries every required feature at runtime rather than inferring support from the
Vulkan version alone. The canonical dependency expressions are in Khronos's
[Vulkan registry XML](https://raw.githubusercontent.com/KhronosGroup/Vulkan-Docs/main/xml/vk.xml).

## Extension and command matrix

| Public operation | Vulkan implementation | Important contract |
|---|---|---|
| `Device::create(Device&, ...)` | Enumerates the four extension strings above; queries the full feature chain, memory types, and `VkPhysicalDeviceDescriptorHeapPropertiesEXT`; creates one timeline semaphore and two reusable frame contexts | A device missing any required extension, feature, combined graphics/compute queue, Vulkan 1.4, coherent device-local host-visible memory, or non-host-visible device-local memory is skipped; if none remains, the factory returns `Error::unsupported`. The device is single-threaded. |
| `Device::gpu_malloc_resource_heap()` / `gpu_malloc_sampler_heap()` | Creates one exact-sized descriptor-capable `VkBuffer` and one dedicated coherent mapped device-memory allocation per call; the private buffer range includes alignment padding and the Vulkan-required hidden reserved suffix | The returned CPU/GPU pointers both name user byte zero and `GpuAllocation::size` is exactly the requested user size. Descriptor heaps do not consume 256 MiB pooled pages. The caller chooses slots, owns the heap, and frees it only after all GPU use completes. |
| `Device::create_texture(Texture&, ...)` | Creates an optimal-tiled `VkImage` and normally suballocates its memory from a 256 MiB GPU-only image heap; driver-required or over-page allocations use the dedicated-allocation path | It writes no descriptor and submits no initialization work. The next command-list begin batches the sole `UNDEFINED` to `GENERAL` metadata transition for all newly created textures. Only attachment-capable textures create a `VkImageView`. |
| `Device::write_texture_descriptor(...)` | Calls `vkWriteResourceDescriptorsEXT` for a sampled/storage `VkImageDescriptorInfoEXT` at the caller's CPU destination | The destination is a caller-selected slot in a live resource-heap allocation. `TextureViewDesc::mip_count == 0` means every remaining mip. Both descriptor kinds use `GENERAL`. |
| `Device::write_sampler_descriptor(...)` | Calls `vkWriteSamplerDescriptorsEXT` from `SamplerDesc` at the caller's CPU destination | The destination is a caller-selected slot in a live sampler-heap allocation. No public sampler object or Vulkan sampler handle is created. |
| `Device::begin_commands(CommandList&)` | Reuses one of two persistent frame-context command pools and primary command buffers after its timeline value has completed | It does not create a command pool or command buffer. No descriptor heap is bound automatically. At most two contexts can be recording or in flight before reuse waits for completion. |
| `CommandList::set_resource_heap()` / `set_sampler_heap()` | Calls `vkCmdBindResourceHeapEXT` / `vkCmdBindSamplerHeapEXT` for the supplied `GpuRange`, computing the hidden reserved suffix from device properties | Heap binding is explicit command state. The range must be the unchanged user GPU pointer/size returned by the matching heap allocation so that the computed suffix names actual backing storage. |
| `CommandList::draw*()` / `dispatch*()` | A typed root pointer pushes `sizeof(Root)` bytes with `vkCmdPushDataEXT`; `nullptr` skips push data. The corresponding Vulkan draw or dispatch follows immediately. | Vulkan copies non-null root bytes while recording, so the CPU root need only remain valid for the method call. Its size must fit `DeviceCaps::max_push_data_size`. A rootless shader declares no `PushConstant` block. There is no separate public push-data command. |
| Graphics/compute pipeline creation | `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` in `VkPipelineCreateFlags2CreateInfo`; `layout = VK_NULL_HANDLE` | Output-first factories return `Error::invalid_shader` for rejected shader modules. Heap-using shaders contain native `SPV_EXT_descriptor_heap` operations and no set/binding decorations. |
| `Device::gpu_malloc(..., MemoryType::default_)`, `MemoryType::gpu_only`, or `MemoryType::readback`; `gpu_free()` | 256 MiB fully bound universal `VkBuffer` pages, persistent mapping where applicable, `vkGetBufferDeviceAddress`, and pinned [OffsetAllocator](https://github.com/sebbbi/OffsetAllocator) suballocation | Malloc defaults to coherent mapped device-local memory and returns the `GpuAllocation {cpu_ptr, gpu_ptr, size}` POD. Default/readback have both pointers; GPU-only has a null `cpu_ptr`; GPU exhaustion returns the all-zero null POD. Free requires the unchanged POD. |
| `CommandList::draw_indexed()` | `vkCmdBindIndexBuffer3KHR`, then `vkCmdDrawIndexed` | The supplied raw `GpuRange` is passed as the address range; its 2/4-byte alignment and draw bounds are asserted, but no allocation is looked up or retained. |
| `CommandList::draw_indirect()` / `draw_indexed_indirect()` | `vkCmdDrawIndirect2KHR` / `vkCmdDrawIndexedIndirect2KHR`; the indexed form first calls `vkCmdBindIndexBuffer3KHR` | Raw `GpuRange` operands are passed directly; argument address and stride are four-byte aligned, and the caller guarantees allocation bounds/lifetime. |
| `CommandList::dispatch_indirect()` | `vkCmdDispatchIndirect2KHR` | The raw range must cover one dispatch command and be four-byte aligned; it is not retained. |
| `CommandList::copy_memory()`, `copy_memory_to_texture()`, `copy_texture_to_memory()` | `vkCmdCopyMemoryKHR`, `vkCmdCopyMemoryToImageKHR`, `vkCmdCopyImageToMemoryKHR` | Each memory operand is a raw `GpuRange`; every image operand uses `GENERAL`. |
| `CommandList::barrier()` | `vkCmdPipelineBarrier2` with a global memory barrier | Callers synchronize actual hazards without naming an image layout. Incompatible stage/access pairs assert. `Access::descriptor_read` maps to both heap-read access bits. |
| `Device::submit()` | Ends the command buffer if needed and submits with `vkQueueSubmit2`, signaling the device timeline | Returns after queueing without waiting. The frame context becomes reusable only after its timeline value completes. |
| `Device::submit_and_wait()` | Performs the same timeline submission, waits for that value, and resets every completed frame pool | Synchronous convenience path; completion covers all earlier queue submissions and pool reset releases their descriptor-heap reserved ranges. No per-submit fence is created. |

All descriptors, dynamic-rendering attachments, and copy operands name `VK_IMAGE_LAYOUT_GENERAL`.
Vulkan still requires a newly created image to start in `VK_IMAGE_LAYOUT_UNDEFINED` and transition
away from it once to initialize image metadata. `create_texture()` leaves that work pending;
`begin_commands()` batches every pending image into one dependency before application commands.
The public API never exposes an image layout or transition, and texture creation never submits or
waits.

The public memory surface is exactly two trivial, standard-layout aggregates:

```cpp
struct GpuRange
{
    void* gpu_ptr;
    std::uint64_t size;
};

struct GpuAllocation
{
    void* cpu_ptr;
    void* gpu_ptr;
    std::uint64_t size;
};
```

There are no constructors, destructors, ownership methods, or PIMPL state in either type.
`gpu_malloc()` returns `GpuAllocation`, `gpu_free()` consumes the unchanged value, and every public
operation that needs a Vulkan address-plus-size pair consumes `GpuRange`. GPU memory exhaustion
returns the canonical null allocation with both pointers and size zero. A successful GPU-only
allocation has a null `cpu_ptr`, so allocation success is determined from `gpu_ptr`.

Command recording performs no address-to-allocation lookup and command lists retain no allocation
records. Address commands consume the numeric pointer and size directly. The caller guarantees that
every range is inside a live allocation and keeps it alive until GPU completion. `gpu_free()` is
immediate from the allocator's point of view and must not be used as deferred GPU lifetime
management.

The allocator maintains separate page pools for `MemoryType::default_`, `gpu_only`, and `readback`. Every
page is one 256 MiB fully bound, non-sparse Vulkan buffer with shader-address, storage, index,
indirect, transfer-source, and transfer-destination usage. A pinned revision of
[OffsetAllocator](https://github.com/sebbbi/OffsetAllocator) manages non-overlapping aligned
suballocations inside each page; when no existing page has enough space, the allocator creates
another 256 MiB page in that memory pool. A single allocation, including alignment padding, must fit
one page. Device selection therefore also requires a 256 MiB mapped coherent device-local heap, a
256 MiB non-host-visible device-local heap, and `maxBufferSize >= 256 MiB`.

Descriptor heaps are intentionally outside those page pools. Each
`gpu_malloc_resource_heap()` or `gpu_malloc_sampler_heap()` call creates one descriptor-capable
buffer sized for the aligned application bytes, suffix padding, and the Vulkan-required reserved
region, then binds one dedicated allocation of coherent mapped device-local memory. This avoids
adding `VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT` to every 256 MiB page: Vulkan permits creation of
such a buffer to fail when its size exceeds the reported descriptor-heap limits. Only the user
range is returned, so heap slot zero remains the returned CPU/GPU pointer and each heap can be
written, selected, and freed independently.

Textures normally use another OffsetAllocator-backed pool of 256 MiB GPU-only image-memory heaps. A
texture still owns its `VkImage`, but ordinary image memory is a pooled suballocation rather than one
allocation per texture. Vulkan images that require dedicated allocation, or cannot fit one page,
retain the Vulkan-required dedicated-memory path. Sampled and storage descriptors use image create
information directly. Only color or depth attachment usage causes creation of the fixed-function
`VkImageView` required by dynamic rendering.

Because every backing buffer is fully bound and has storage-buffer usage, address commands
consistently use
`VK_ADDRESS_COMMAND_FULLY_BOUND_BIT_KHR | VK_ADDRESS_COMMAND_STORAGE_BUFFER_USAGE_BIT_KHR`.
The exact flag and aliasing rules are in the
[device-address command proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_device_address_commands.html).

Texture creation queries the exact
optimal-tiling format/type/usage combination and checks its returned extent, mip, sample-count,
and resource-size limits. Sampled formats must support sampled-image access; the API does not impose
linear-filter support when a texture may only be used with nearest filtering. Storage formats must
support formatless reads and writes used by general Slang `RWTexture` declarations. Graphics pipeline
color/depth formats are separately checked for attachment support.

The public 8-bit color formats are `rgba8_unorm`, `rgba8_srgb`, `bgra8_unorm`, and
`bgra8_srgb`, mapped directly to their Vulkan `R8G8B8A8`/`B8G8R8A8` UNORM/SRGB formats. The triangle
uses a `bgra8_srgb` color target. The cube uploads the LunarG logo as `rgba8_srgb`; its
`bgra8_unorm` output target receives the reference sample's explicit linear-to-sRGB conversion.

`MemoryType::gpu_only` allocations and textures require a non-host-visible device-local memory type and
are never mapped. This is enforced independently from the coherent device-local type used by
default allocations, readback, and descriptor-heap storage.

All device, allocation, descriptor, command-recording, and submission state is deliberately
single-threaded. The implementation uses no mutexes or atomic counters. Calling methods on one
device concurrently, including nominally `const` methods, is outside the API contract.

## Descriptor-heap and root-data model

The heap allocator uses the portable class sizes from
`VkPhysicalDeviceDescriptorHeapPropertiesEXT`: `imageDescriptorSize` for the resource heap and
`samplerDescriptorSize` for the sampler heap. It intentionally does not tightly pack using
`vkGetPhysicalDeviceDescriptorSizeEXT`; Khronos describes that query as a specialized,
non-portable optimization. Heap base addresses, descriptors, and reserved regions honor all
reported alignments and limits. The normative rules are in the
[descriptor-heaps chapter](https://docs.vulkan.org/spec/latest/chapters/descriptorheaps.html) and
[`VkPhysicalDeviceDescriptorHeapPropertiesEXT`](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceDescriptorHeapPropertiesEXT.html).

The application passes `N * DeviceCaps::image_descriptor_size` bytes to
`gpu_malloc_resource_heap()`, or `N * DeviceCaps::sampler_descriptor_size` bytes to
`gpu_malloc_sampler_heap()`. The returned `cpu_ptr`, `gpu_ptr`, and `size` describe only those `N`
user slots. Each dedicated backing buffer appends the required reserved bytes as an
implementation-owned suffix. Consequently application slot zero is exactly the returned pointer
and shader index zero; there is no reserved-prefix index adjustment.

`Device::write_texture_descriptor()` accepts a caller-selected CPU destination, a live `Texture`,
`TextureDescriptorType::sampled` or `storage`, and an optional `TextureViewDesc`. It writes one
device-sized image descriptor directly at that address. `Device::write_sampler_descriptor()` does
the same for a `SamplerDesc`; there is no public `Sampler` handle. Applications can arrange slots,
contiguous tables, and multiple texture views freely.

`CommandList::set_resource_heap()` and `set_sampler_heap()` explicitly bind the application
allocations. The supplied `GpuRange` uses the unchanged user `gpu_ptr` and `size`; the backend
computes the reserved suffix offset and size from device properties for `VkBindHeapInfoEXT`. Neither
heap is created or selected by `Device::begin_commands()`.

Descriptors are written into coherent device-local mapped storage. The runtime rejects devices
without a `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT` memory type, so explicit mapped-range flushes
and invalidations do not exist. GPU writes or copies into a heap must be synchronized to the
appropriate sampler/resource heap read access; descriptor availability does not replace
synchronization for the described texture itself.

The backend does not retain heaps, descriptor slots, textures, allocations, or pipelines. Heap
contents and GPU pointers copied from root data are opaque, so the caller must keep every referenced
object and GPU allocation alive and keep descriptors unchanged until the submission has completed.
`submit_and_wait()` gives an immediate completion-and-reset point; after `submit()`, use a later
`submit_and_wait()` or `wait_idle()` before freeing or reusing GPU storage. Resetting completed
command pools is required before descriptor-heap reserved ranges can be released.

Shared root structures are ordinary CPU PODs and may contain typed BDA pointers represented by the
same pointer type in C++ and Slang. A draw or dispatch takes its optional root first. Passing
`nullptr` emits no push data; passing `const Root*` immediately copies `sizeof(Root)` bytes into
command-buffer push data. The CPU root therefore requires no GPU allocation and no lifetime beyond
that call. GPU pointer fields must not be dereferenced by the CPU,
and the allocations they name must remain alive through GPU completion. A root can mix typed BDA
pointers, `uint32` texture/sampler indices, and 32-bit scalar/vector/matrix values. Slang shaders
sharing C++ POD structures use `-fvk-use-c-layout`; shaders sharing matrix PODs additionally use
`-matrix-layout-row-major`. `shader_types.h` supplies compact row-major `float2x2`, `float3x3`,
`float4x4`, and `float3x4` C++ PODs matching Slang's row-by-column names. The triangle is rootless.
The cube root contains a typed vertex pointer, one user-chosen resource index, explicit padding,
and a `float4x4` transform. Sampler selection uses the shared `CubeSampler` enum rather than a root
field. Its four values `wrap_linear`, `wrap_point`, `clamp_linear`, and `clamp_point` map to sampler
slots 0 through 3.

Shaders that access the heaps compile with capability
`spvDescriptorHeapEXT` and use Slang's `ResourceDescriptorHeap[index]` and
`SamplerDescriptorHeap[index]`. The cube uses this path; the triangle has neither a root nor heap
syntax. The native heap path maps to
[`SPV_EXT_descriptor_heap`](https://github.khronos.org/SPIRV-Registry/extensions/EXT/SPV_EXT_descriptor_heap.html),
whose `DescriptorHeapEXT` capability implicitly declares `UntypedPointersKHR`. Slang 2026.14.1 or
newer and this capability are mandatory, so heap-using shaders contain direct heap instructions.

Heap commands and descriptor-set, descriptor-buffer, push-descriptor, or push-constant commands
mutually invalidate their state. The backend never records `vkCmdBindDescriptorSets`,
descriptor-buffer commands, push descriptors, or
`vkCmdPushConstants`, so a `clean_gfx` command list stays entirely in heap mode. See
[`vkCmdPushDataEXT`](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdPushDataEXT.html)
for the host-copy contract and the descriptor-heaps chapter for the full invalidation list.

## Command recording and submission

Each device owns two persistent frame contexts. A context contains one reusable command pool, one
primary command buffer, and the timeline value of its most recent submission. `begin_commands()`
selects the next context, waits only when that context's earlier timeline value is still in flight,
resets its persistent pool, and begins its existing command buffer. It does not allocate or destroy
a pool or command buffer per recording.

`Device::submit()` ends recording when necessary and queues the command buffer with `vkQueueSubmit2`,
signaling a monotonically increasing value on the device's timeline semaphore. It returns after the
queue submission, without waiting for execution. `submit_and_wait()` performs the same submission
and then waits for that value. It resets all completed frame pools after the wait. No per-submit
`VkFence` is created. `wait_idle()` remains the explicit whole-device completion point and likewise
resets inactive frame pools before returning.

The two contexts permit two submissions to be in flight before context reuse can wait. They are an
internal command-memory lifecycle mechanism, not resource lifetime tracking: recording or submission
does not retain any public object or allocation. Callers using asynchronous `submit()` must preserve
all referenced memory, textures, pipelines, descriptor heaps, and descriptor contents until a later
completion point. The queue, contexts, timeline counters, and allocators are single-threaded and use
no locks or atomics.

## Example coverage and presentation

The triangle is a 512x512 minimal RGB rendering following the first triangle in the official
[Khronos Vulkan Tutorial](https://docs.vulkan.org/tutorial/latest/03_Drawing_a_triangle/02_Graphics_pipeline_basics/01_Shader_modules.html).
It issues one `draw(nullptr, 3)` and selects three constant positions/colors in the vertex shader
using `SV_VertexID`. It has no vertex allocation, root block, texture, descriptor index, compute
stage, or depth attachment.

The cube is a 500x500 rendering based on the official
[Khronos/LunarG `vkcube` source](https://github.com/KhronosGroup/Vulkan-Tools/tree/vulkan-sdk-1.4.357.0/cube).
It uses BDA vertex fetch, sampled-image and sampler heaps, an sRGB texture, a depth attachment, and
the sample's four-degree-per-frame Y rotation. It allocates and binds both heaps explicitly, writes
the texture into resource slot zero, and writes the four shared `CubeSampler` variants into sampler
slots 0 through 3. Its embedded texture is the exact
[`lunarg.ppm.h`](https://github.com/KhronosGroup/Vulkan-Tools/blob/vulkan-sdk-1.4.357.0/cube/lunarg.ppm.h)
asset distributed under Vulkan-Tools' [Apache-2.0 license](https://github.com/KhronosGroup/Vulkan-Tools/blob/vulkan-sdk-1.4.357.0/LICENSE.txt).

The core `gfx` implementation remains WSI-free and requires no surface or swapchain extension. The
examples render to ordinary Vulkan textures and use device-address copies into coherent readback
memory. On Windows, running without an argument displays those CPU-visible BGRA pixels in a Win32
window with a GDI blit; this presentation helper is example-only. Passing a PPM path selects
one-frame file output instead, which is useful for deterministic checks and headless runs.

## Driver evidence

Support must always be decided by extension enumeration and feature queries, not vendor or version
strings. The following is primary-source evidence available on the status date, not a guarantee for
every GPU in a driver's product matrix.

| Driver/backend | Evidence as of 2026-08-15 |
|---|---|
| NVIDIA proprietary | NVIDIA's [Vulkan developer-driver history](https://developer.nvidia.com/vulkan-driver) added `VK_EXT_descriptor_heap` in Windows 582.30/Linux 580.94.16 on 2026-01-23 and `VK_KHR_device_address_commands` in Windows 595.92/Linux 595.44.03 on 2026-03-13. NVIDIA's [descriptor-heap guidance](https://developer.nvidia.com/blog/streamlining-resource-binding-with-end-to-end-support-for-vulkan-descriptor-heaps/) recommends driver 610+ for the full descriptor-heap/tooling path; earlier access is through the developer beta branch. |
| AMD Windows | AMD's [Vulkan driver support table](https://www.amd.com/en/resources/support-articles/release-notes/rn-rad-win-vulkan.html) lists `VK_EXT_descriptor_heap` in 25.30.17.02 and lists both it and `VK_KHR_device_address_commands` in Adrenalin 26.6.1 (2026-06-02). |
| Intel ANV / AMD RADV on Linux | [Mesa 26.1](https://docs.mesa3d.org/relnotes/26.1.0.html) introduced device-address commands on RADV and descriptor heaps behind `RADV_EXPERIMENTAL=heap`. [Mesa 26.2](https://docs.mesa3d.org/relnotes/26.2.0.html) enables descriptor heaps by default on ANV and RADV, and its change list records the ANV device-address-commands implementation. |
| Intel Windows | No Intel Windows release note naming the complete required extension set was found. Runtime enumeration remains authoritative. |

## Remaining deviations from the proposal

- Device-address commands expose only `GpuRange` pointer/size PODs rather than buffer handles, but
  Vulkan allocations, 256 MiB backing buffers for ordinary value memory, exact-sized dedicated
  descriptor buffers, usage flags, and lifetimes still exist internally.
  Command recording itself remains CPU-driven unless a future device-generated-commands layer is
  added.
- The triangle generates vertices from `SV_VertexID`, while the cube manually fetches vertices
  through its root's typed GPU pointer, so neither example needs vertex-buffer binding. Indexed and
  indirect draws, indirect dispatch, and copy paths use addresses today, but
  the wrapper does not yet expose indirect-count-buffer variants, `vkCmdBindVertexBuffers3KHR`,
  address-range barriers, address-based fill/update, or device-generated commands.
- Textures remain `VkImage` objects with pooled allocation, usage, and lifetime rules; attachment
  textures additionally need a `VkImageView`. Rendering still uses Vulkan pipeline objects and
  dynamic-rendering attachment state.
- Descriptor encodings are produced by explicit host helper calls into application-owned mapped
  heaps. The runtime does not generate, compact, or relocate descriptor bytes on the GPU.
