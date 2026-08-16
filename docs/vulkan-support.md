# Vulkan support and design mapping

Status date: 2026-08-15.

`clean_gfx` is a Vulkan-only implementation of the design proposed in
[No Graphics API](https://www.sebastianaaltonen.com/blog/no-graphics-api): small CPU-pushed root
data, buffer device-address pointers, and 32-bit indices into global texture and sampler heaps.
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
deliberately requires a Vulkan 1.4 loader and physical device. It builds against Vulkan-Headers
1.4.357 or newer.

| Requirement | Enabled feature/API | Why `clean_gfx` requires it |
|---|---|---|
| Vulkan 1.4 | `maintenance5` | Provides the 64-bit pipeline-create flags path used for descriptor-heap pipelines. |
| [`VK_EXT_descriptor_heap`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_descriptor_heap.html) | `VkPhysicalDeviceDescriptorHeapFeaturesEXT::descriptorHeap` | Two global heaps, host descriptor generation, `vkCmdPushDataEXT`, and pipelines with no layout. Capture/replay is not enabled. |
| [`VK_KHR_shader_untyped_pointers`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_untyped_pointers.html) | `shaderUntypedPointers` | Native `ResourceDescriptorHeap`/`SamplerDescriptorHeap` access. The implementation deliberately requires and enables it even when the shader compiler takes the mapping fallback; Slang BDA pointers use the separate physical-storage-buffer path. |
| [`VK_KHR_device_address_commands`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_device_address_commands.html) | `deviceAddressCommands` | Address-range index binding, indirect draw/dispatch, and buffer/image copies. |
| [`VK_KHR_unified_image_layouts`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_unified_image_layouts.html) | `unifiedImageLayouts` | Guarantees that `VK_IMAGE_LAYOUT_GENERAL` is efficient for every normal image use, eliminating public layout-state tracking. Video layouts are out of scope, so `unifiedImageLayoutsVideo` is not enabled. |
| [Buffer device address](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_buffer_device_address.html) | `bufferDeviceAddress` | `GpuAllocation`/`GpuRange` addresses, internal heap-page addresses, and typed pointers in shared root structures. |
| Vulkan 1.3 | `synchronization2`, `dynamicRendering` | Barriers and rendering without render-pass/framebuffer objects. |
| Vulkan 1.2 | `shaderFloat16`, `runtimeDescriptorArray`, `scalarBlockLayout` | Half-precision shared values, mapping-fallback arrays, and C-compatible shared layouts. |
| Vulkan 1.1/core | `storageBuffer16BitAccess`, `storagePushConstant16`, `shaderDrawParameters`, `shaderInt16`, graphics-stage stores/atomics, and storage-image reads/writes without a shader format | 16-bit members in pointed-to storage and pushed root data, Slang's `SV_VertexID` lowering, and general sampled/storage texture use from graphics and compute shaders. |
| Host/queue | 64-bit little-endian host, IEEE-754 binary32, graphics-and-compute-capable queue | Shared pointer representation and the current single-queue command implementation. |
| Mapped device memory | A memory type and at least 256 MiB device-local heap with `DEVICE_LOCAL`, `HOST_VISIBLE`, and `HOST_COHERENT` | Upload, readback, and descriptor-heap storage is persistently mapped without flush/invalidate operations. `HOST_CACHED` is preferred for readback but is not required. |
| GPU-only device memory | A memory type on an at least 256 MiB heap with `DEVICE_LOCAL` and without `HOST_VISIBLE` | GPU-only allocations and textures cannot fall back to CPU-visible memory. |

`VK_EXT_debug_utils` and `VK_LAYER_KHRONOS_validation` are optional. `DeviceDesc::enable_validation`
uses them when present.

## Error model

`clean_gfx` has no C++ exception path and all repository targets are compiled with exception
handling disabled. The public runtime result is deliberately small:

```cpp
enum class Error : std::uint8_t
{
    none,
    unsupported,
    out_of_device_memory,
    out_of_descriptors,
    invalid_shader,
    device_lost,
    driver_error,
};
```

Fallible object factories take their output object first and return `Error`; the output remains
empty on failure. `CommandList::finish()`, `Device::submit_and_wait()`, and `Device::wait_idle()`
also return `Error`. Incorrect handles, ranges, alignment, state transitions, and other programming
errors are assertions rather than recoverable runtime results. CPU allocation failure is not
handled. GPU heap exhaustion is reported directly by `gpu_malloc()` as
`GpuAllocation {nullptr, nullptr, 0}`; a caller that wants to recover tests `gpu_ptr`.

For reference, the registry dependency for `VK_EXT_descriptor_heap` is
`((VK_KHR_extended_flags or VK_KHR_maintenance5) and (VK_KHR_buffer_device_address or Vulkan 1.2))
or Vulkan 1.4`. `VK_KHR_device_address_commands` can be supported on Vulkan 1.3, or on an older
stack with its synchronization, dynamic-state, and BDA dependencies. The repository chooses the
simpler Vulkan 1.4 baseline and still queries every feature at runtime. The canonical dependency
expressions are in Khronos's [Vulkan registry XML](https://raw.githubusercontent.com/KhronosGroup/Vulkan-Docs/main/xml/vk.xml).

## Extension and command matrix

| Public operation | Vulkan implementation | Important contract |
|---|---|---|
| `Device::create(Device&, ...)` | Enumerates the four extension strings above; queries the full feature chain, memory types, and `VkPhysicalDeviceDescriptorHeapPropertiesEXT` | A device missing any required extension, feature, combined graphics/compute queue, Vulkan 1.4, coherent device-local host-visible memory, or non-host-visible device-local memory is skipped; if none remains, the factory returns `Error::unsupported`. |
| Global heaps | Coherent device-local host-visible buffers with `VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT`, `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`, and transfer usage; memory uses `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT` | One resource heap and one sampler heap live for the device lifetime. The implementation-reserved range starts at byte zero; user slots start at the aligned `DeviceCaps::first_texture_index` and `first_sampler_index`. |
| `Device::create_texture(Texture&, ...)` | One private `UNDEFINED` to `GENERAL` initialization, then `vkWriteResourceDescriptorsEXT` with sampled/storage `VkImageDescriptorInfoEXT` | Both descriptor kinds use `GENERAL`. Each view consumes an `imageDescriptorSize` slot. `Texture::sampled_index()` and `storage_index()` return the physical heap index, including the reserved prefix. |
| `Device::create_sampler(Sampler&, ...)` | `vkWriteSamplerDescriptorsEXT` from `VkSamplerCreateInfo` | `Sampler::index()` addresses a `samplerDescriptorSize` slot. No Vulkan sampler handle is needed for the shader descriptor. |
| `Device::begin_commands(CommandList&)` | Creates a command-list-local pool, then calls `vkCmdBindSamplerHeapEXT` and `vkCmdBindResourceHeapEXT` | Per-list pools allow independent host recording; both long-lived heaps and the same exact reserved ranges are bound once at command-buffer start. |
| `CommandList::push_data()` / `push_root()` | `vkCmdPushDataEXT` with `VkPushDataInfoEXT` | Source data is a host pointer. Offset and byte count must be multiples of four, and the end must not exceed `DeviceCaps::max_push_data_size`. All shader stages see the same `PushConstant` block. |
| Graphics/compute pipeline creation | `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` in `VkPipelineCreateFlags2CreateInfo`; `layout = VK_NULL_HANDLE` | Output-first factories return `Error::invalid_shader` for rejected shader modules. Standard heap mappings are attached per shader stage and are ignored by native direct-heap shaders that have no set/binding decorations. |
| `Device::gpu_malloc()` / `gpu_free()` | 256 MiB fully bound universal `VkBuffer` pages, persistent mapping where applicable, `vkGetBufferDeviceAddress`, and pinned [OffsetAllocator](https://github.com/sebbbi/OffsetAllocator) suballocation | Malloc returns the `GpuAllocation {cpu_ptr, gpu_ptr, size}` POD. Upload/readback have both pointers; GPU-only has a null `cpu_ptr`; GPU exhaustion returns the all-zero null POD. Free requires the unchanged POD. |
| `CommandList::draw_indexed()` | `vkCmdBindIndexBuffer3KHR`, then `vkCmdDrawIndexed` | The supplied `GpuRange` is resolved through the hidden allocation registry; its byte range and 2/4-byte alignment are asserted. |
| `CommandList::draw_indirect()` / `draw_indexed_indirect()` | `vkCmdDrawIndirect2KHR` / `vkCmdDrawIndexedIndirect2KHR`; the indexed form first calls `vkCmdBindIndexBuffer3KHR` | `GpuRange` operands are resolved and retained; argument address and stride are four-byte aligned, and the live allocation must cover every requested command. |
| `CommandList::dispatch_indirect()` | `vkCmdDispatchIndirect2KHR` | The `GpuRange` is resolved and retained, must cover one dispatch command, and is four-byte aligned. |
| `CommandList::copy_memory()`, `copy_memory_to_texture()`, `copy_texture_to_memory()` | `vkCmdCopyMemoryKHR`, `vkCmdCopyMemoryToImageKHR`, `vkCmdCopyImageToMemoryKHR` | Each memory operand is an explicit `GpuRange` resolved to a live allocation; every image operand uses `GENERAL`. |
| `CommandList::barrier()` | `vkCmdPipelineBarrier2` with a global memory barrier | Callers synchronize actual hazards without naming an image layout. Incompatible stage/access pairs assert. `Access::descriptor_read` maps to both heap-read access bits. |

All descriptors, dynamic-rendering attachments, and copy operands name `VK_IMAGE_LAYOUT_GENERAL`.
Vulkan still requires a newly created image to start in `VK_IMAGE_LAYOUT_UNDEFINED` and transition
away from it once to initialize image metadata. `create_texture()` records that sole transition and
waits for it before returning the texture. The public API never exposes an image layout or transition.
This synchronous setup submit matches the prototype's current allocation/submission model; a future
allocator can batch image initialization without changing the API.

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

The allocator maintains separate page pools for `MemoryType::upload`, `gpu`, and `readback`. Every
page is one 256 MiB fully bound, non-sparse Vulkan buffer with shader-address, storage, index,
indirect, transfer-source, and transfer-destination usage. A pinned revision of
[OffsetAllocator](https://github.com/sebbbi/OffsetAllocator) manages non-overlapping aligned
suballocations inside each page; when no existing page has enough space, the allocator creates
another 256 MiB page in that memory pool. A single allocation, including alignment padding, must fit
one page. Device selection therefore also requires a 256 MiB mapped coherent device-local heap, a
256 MiB non-host-visible device-local heap, and `maxBufferSize >= 256 MiB`.

Because every backing buffer is fully bound and has storage-buffer usage, address commands
consistently use
`VK_ADDRESS_COMMAND_FULLY_BOUND_BIT_KHR | VK_ADDRESS_COMMAND_STORAGE_BUFFER_USAGE_BIT_KHR`.
The exact flag and aliasing rules are in the
[device-address command proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_device_address_commands.html).

Texture creation queries the exact
optimal-tiling format/type/usage combination and checks its returned extent, mip, sample-count,
and resource-size limits. To keep global texture/sampler indices freely composable, sampled formats
must support linear filtering even if a particular shader uses a nearest sampler; storage formats
must support formatless reads and writes used by general Slang `RWTexture` declarations. Graphics
pipeline color/depth formats are separately checked for attachment support.

The public 8-bit color formats are `rgba8_unorm`, `rgba8_srgb`, `bgra8_unorm`, and
`bgra8_srgb`, mapped directly to their Vulkan `R8G8B8A8`/`B8G8R8A8` UNORM/SRGB formats. The triangle
uses a `bgra8_srgb` color target. The cube uploads the LunarG logo as `rgba8_srgb`; its
`bgra8_unorm` output target receives the reference sample's explicit linear-to-sRGB conversion.

`MemoryType::gpu` allocations and textures require a non-host-visible device-local memory type and
are never mapped. This is enforced independently from the coherent device-local type used by
upload, readback, and descriptor-heap storage.

## Descriptor-heap and root-data model

The heap allocator uses the portable class sizes from
`VkPhysicalDeviceDescriptorHeapPropertiesEXT`: `imageDescriptorSize` for the resource heap and
`samplerDescriptorSize` for the sampler heap. It intentionally does not tightly pack using
`vkGetPhysicalDeviceDescriptorSizeEXT`; Khronos describes that query as a specialized,
non-portable optimization. Heap base addresses, descriptors, and reserved regions honor all
reported alignments and limits. The normative rules are in the
[descriptor-heaps chapter](https://docs.vulkan.org/spec/latest/chapters/descriptorheaps.html) and
[`VkPhysicalDeviceDescriptorHeapPropertiesEXT`](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceDescriptorHeapPropertiesEXT.html).
The same reserved range is reused for every command list and its bytes are never exposed to the
application. `maxPushDataSize` is queried rather than assumed (the extension guarantees at least
256 bytes).

Descriptors are written directly into coherent device-local mapped heap storage. The runtime rejects
devices without a `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT` memory type, so explicit mapped-range
flushes and invalidations do not exist in the API or backend.
Future GPU writes or copies into a heap must be synchronized to the appropriate sampler/resource
heap read access; descriptor availability does not replace synchronization for the resource
described.

Address-based command methods accept `GpuRange` PODs. A hidden registry resolves the range's base or
interior GPU pointer, asserts that the explicit byte range remains inside a live allocation, and
retains the corresponding suballocation record. `gpu_free()` removes the allocation from that live
registry, but OffsetAllocator does not receive its offset back until the last recorded command-list
reference is released; with synchronous submission, that happens after the submitted list has
finished. Command lists likewise retain pipelines and attachment/copy textures. They cannot inspect
opaque root bytes, so allocations referenced only by a BDA field and textures/samplers referenced
only by a heap index must remain alive until `submit_and_wait()` returns. Releasing such a resource
may invalidate the pointer or reuse its descriptor slot.

Shared root structures use ordinary typed pointers for BDA values in both C++ and Slang. On the CPU,
the pointer value carries GPU virtual-address bits and must not be dereferenced. A root POD can mix
those typed BDA pointers, `uint32` texture/sampler indices, and 16/32-bit scalar/vector values.
`push_root()` requires a standard-layout, trivially copyable type with four-byte-multiple size, while
shared examples add explicit
`sizeof`, `alignof`, and `offsetof` assertions. Slang is invoked with `-fvk-use-c-layout` when the
compiler exposes it. The triangle root is one 64-bit typed pointer and always uses C layout. The
cube's current asserted POD layouts also agree with its older-compiler scalar-layout fallback.

For shaders that access the heaps, the preferred path compiles with capability
`spvDescriptorHeapEXT` and uses Slang's `ResourceDescriptorHeap[index]` and
`SamplerDescriptorHeap[index]`. The cube uses this path; the triangle has only a BDA pointer and
needs neither heap syntax nor descriptor mappings. The native heap path maps to
[`SPV_EXT_descriptor_heap`](https://github.khronos.org/SPIRV-Registry/extensions/EXT/SPV_EXT_descriptor_heap.html),
whose `DescriptorHeapEXT` capability implicitly declares `UntypedPointersKHR`. If `slangc -h` does
not advertise that capability, the backend uses this binding-mapping convention:

- set 0, binding 0: sampled-image array;
- set 0, binding 1: sampler array;
- set 0, binding 2: storage-image array.

The current cube fallback exercises bindings 0 and 1 for its sampled sRGB texture and sampler.
Binding 2 remains part of the backend convention for application storage images, but neither
current example uses it.

`VkShaderDescriptorSetAndBindingMappingInfoEXT` maps those declarations to constant-offset heap
ranges. This fallback still creates no descriptor-set layout, pool, set, or pipeline layout.

Heap commands and legacy descriptor/push-constant commands mutually invalidate their state. The
backend never records `vkCmdBindDescriptorSets`, descriptor-buffer commands, push descriptors, or
`vkCmdPushConstants`, so a `clean_gfx` command list stays entirely in heap mode. See
[`vkCmdPushDataEXT`](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdPushDataEXT.html)
for the host-copy contract and the descriptor-heaps chapter for the full invalidation list.

## Example coverage and presentation

The triangle is a 512x512 minimal RGB rendering of the official
[Khronos Hello Triangle 1.3](https://github.com/KhronosGroup/Vulkan-Samples/tree/main/samples/api/hello_triangle_1_3).
It issues one non-indexed draw, fetches its three vertices through the root's typed BDA pointer, and
uses no texture, descriptor index, compute stage, or depth attachment.

The cube is a 500x500 rendering based on the official
[Khronos/LunarG `vkcube` source](https://github.com/KhronosGroup/Vulkan-Tools/tree/vulkan-sdk-1.4.357.0/cube).
It uses BDA vertex fetch, sampled-image and sampler heaps, an sRGB texture, a depth attachment, and
the sample's four-degree-per-frame Y rotation. Its embedded texture is the exact
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

- `vkCmdPushDataEXT` copies the root bytes from CPU memory. It cannot fetch an arbitrary root
  structure through a GPU address. Larger data is reached through BDA pointers stored in the root.
- Device-address commands expose only `GpuRange` pointer/size PODs rather than buffer handles, but
  Vulkan allocations, 256 MiB backing buffers, usage flags, and lifetimes still exist internally.
  Command recording itself remains CPU-driven unless a future device-generated-commands layer is
  added.
- Vertex data is manually fetched through the typed root pointer, so no vertex-buffer binding is
  needed. Indexed and indirect draws, indirect dispatch, and copy paths use addresses today, but
  the wrapper does not yet expose indirect-count-buffer variants, `vkCmdBindVertexBuffers3KHR`,
  address-range barriers, address-based fill/update, or device-generated commands.
- Textures remain `VkImage` objects with allocation, view, usage, and lifetime rules. Rendering still
  uses Vulkan pipeline objects and dynamic-rendering attachment state.
- Descriptor encodings are produced by host calls and placed in host-visible heaps. The current
  allocator does not generate, compact, or relocate descriptor bytes on the GPU.
- The Slang binding-mapping fallback retains set/binding decorations in SPIR-V, although it still
  uses the same heaps and has no Vulkan descriptor-set objects. Native `SPV_EXT_descriptor_heap`
  removes those decorations when the installed Slang compiler supports it.
