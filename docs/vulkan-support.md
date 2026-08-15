# Vulkan support and design mapping

Status date: 2026-08-15.

`clean_gfx` is a Vulkan-only implementation of the design proposed in
[No Graphics API](https://www.sebastianaaltonen.com/blog/no-graphics-api): small CPU-pushed root
data, buffer device-address pointers, and 32-bit indices into global texture and sampler heaps.
There are no descriptor sets, descriptor pools, descriptor-set layouts, or pipeline layouts in the
public API or Vulkan backend.

MoltenVK is intentionally unsupported. The current capability check targets desktop Vulkan 1.4
drivers exposing all requirements below; a MoltenVK device that does not expose them is rejected.

`VK_EXT_descriptor_heap` (device extension 136, revision 1) and
`VK_KHR_device_address_commands` (device extension 319, revision 1) are ratified. `vkCmdPushDataEXT`
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
| [Buffer device address](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_buffer_device_address.html) | `bufferDeviceAddress` | `Buffer::address()`, heap addresses, and typed pointers in shared root structures. |
| Vulkan 1.3 | `synchronization2`, `dynamicRendering` | Barriers and rendering without render-pass/framebuffer objects. |
| Vulkan 1.2 | `shaderFloat16`, `runtimeDescriptorArray`, `scalarBlockLayout` | Half-precision shared values, mapping-fallback arrays, and C-compatible shared layouts. |
| Vulkan 1.1/core | `storageBuffer16BitAccess`, `storagePushConstant16`, `shaderDrawParameters`, `shaderInt16`, graphics-stage stores/atomics, and storage-image reads/writes without a shader format | 16-bit members in pointed-to storage and pushed root data, Slang's `SV_VertexID` lowering, and general sampled/storage texture use from graphics and compute shaders. |
| Host/queue | 64-bit little-endian host, IEEE-754 binary32, graphics-and-compute-capable queue | Shared pointer representation and the current single-queue command implementation. |

`VK_EXT_debug_utils` and `VK_LAYER_KHRONOS_validation` are optional. `DeviceDesc::enable_validation`
uses them when present.

For reference, the registry dependency for `VK_EXT_descriptor_heap` is
`((VK_KHR_extended_flags or VK_KHR_maintenance5) and (VK_KHR_buffer_device_address or Vulkan 1.2))
or Vulkan 1.4`. `VK_KHR_device_address_commands` can be supported on Vulkan 1.3, or on an older
stack with its synchronization, dynamic-state, and BDA dependencies. The repository chooses the
simpler Vulkan 1.4 baseline and still queries every feature at runtime. The canonical dependency
expressions are in Khronos's [Vulkan registry XML](https://raw.githubusercontent.com/KhronosGroup/Vulkan-Docs/main/xml/vk.xml).

## Extension and command matrix

| Public operation | Vulkan implementation | Important contract |
|---|---|---|
| `Device::create()` | Enumerates the three extension strings above; queries the full feature chain and `VkPhysicalDeviceDescriptorHeapPropertiesEXT` | A device missing any required extension, feature, combined graphics/compute queue, or Vulkan 1.4 is skipped. |
| Global heaps | Host-visible buffers with `VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT`, `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`, and transfer usage; memory uses `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT` | One resource heap and one sampler heap live for the device lifetime. The implementation-reserved range starts at byte zero; user slots start at the aligned `DeviceCaps::first_texture_index` and `first_sampler_index`. |
| `Device::create_texture()` | `vkWriteResourceDescriptorsEXT` with sampled/storage `VkImageDescriptorInfoEXT` | Each sampled or storage view consumes an `imageDescriptorSize` slot. `Texture::sampled_index()` and `storage_index()` return the physical heap index, including the reserved prefix. |
| `Device::create_sampler()` | `vkWriteSamplerDescriptorsEXT` from `VkSamplerCreateInfo` | `Sampler::index()` addresses a `samplerDescriptorSize` slot. No Vulkan sampler handle is needed for the shader descriptor. |
| `Device::begin_commands()` | Creates a command-list-local pool, then calls `vkCmdBindSamplerHeapEXT` and `vkCmdBindResourceHeapEXT` | Per-list pools allow independent host recording; both long-lived heaps and the same exact reserved ranges are bound once at command-buffer start. |
| `CommandList::push_data()` / `push_root()` | `vkCmdPushDataEXT` with `VkPushDataInfoEXT` | Source data is a host pointer. Offset and byte count must be multiples of four, and the end must not exceed `DeviceCaps::max_push_data_size`. All shader stages see the same `PushConstant` block. |
| Graphics/compute pipeline creation | `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` in `VkPipelineCreateFlags2CreateInfo`; `layout = VK_NULL_HANDLE` | The flag is 64-bit and cannot be placed in the legacy `VkPipelineCreateFlags` field. Standard heap mappings are attached per shader stage and are ignored by native direct-heap shaders that have no set/binding decorations. |
| `Buffer::address()` / `BufferSlice` | `vkGetBufferDeviceAddress` and `VkDeviceAddressRangeKHR` | Every buffer is a separate, fully bound, non-sparse allocation with storage, index, indirect, transfer, and shader-address usage. Command APIs accept only provenance-carrying slices made by `Buffer::slice()` and retain their allocations through submission. Raw addresses are reserved for opaque shader root data. |
| `CommandList::draw_indexed()` | `vkCmdBindIndexBuffer3KHR`, then `vkCmdDrawIndexed` | The index range is a device address plus size; 2/4-byte alignment is checked. |
| `CommandList::draw_indirect()` / `draw_indexed_indirect()` | `vkCmdDrawIndirect2KHR` / `vkCmdDrawIndexedIndirect2KHR`; the indexed form first calls `vkCmdBindIndexBuffer3KHR` | Argument address and stride are four-byte aligned; the stride/range must cover every requested command. |
| `CommandList::dispatch_indirect()` | `vkCmdDispatchIndirect2KHR` | The argument range is a device address, has indirect usage, and is four-byte aligned. |
| `CommandList::copy_buffer()`, `copy_buffer_to_texture()`, `copy_texture_to_buffer()` | `vkCmdCopyMemoryKHR`, `vkCmdCopyMemoryToImageKHR`, `vkCmdCopyImageToMemoryKHR` | Buffer sides are device-address ranges; Vulkan image handles and layouts remain necessary. |
| `CommandList::transition()` / `barrier()` | `vkCmdPipelineBarrier2` with image or global memory barriers | The caller-provided `before` state must be the image's actual layout; invalid destination states, usage mismatches, and incompatible stage/access pairs are rejected. `Access::descriptor_read` maps to both heap-read access bits. |

All public buffers use `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`, do not alias, and are fully bound, so
the current address commands consistently use
`VK_ADDRESS_COMMAND_FULLY_BOUND_BIT_KHR | VK_ADDRESS_COMMAND_STORAGE_BUFFER_USAGE_BIT_KHR`.
If suballocation, sparse binding, protected memory, or aliased buffers are added, this constant must
be replaced by per-allocation `VkAddressCommandFlagsKHR` metadata. The exact aliasing rules are in
the [device-address command proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_device_address_commands.html).

Buffer sizes are checked against Vulkan 1.3's `maxBufferSize`. Texture creation queries the exact
optimal-tiling format/type/usage combination and validates its returned extent, mip, sample-count,
and resource-size limits. To keep global texture/sampler indices freely composable, sampled formats
must support linear filtering even if a particular shader uses a nearest sampler; storage formats
must support formatless reads and writes used by general Slang `RWTexture` declarations. Graphics
pipeline color/depth formats are separately checked for attachment support.

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

Descriptors are written directly into mapped heap storage and non-coherent memory is flushed.
Future GPU writes or copies into a heap must be synchronized to the appropriate sampler/resource
heap read access; descriptor availability does not replace synchronization or image-layout rules
for the resource described.

Command lists retain pipelines, attachment/copy textures, and every provenance-carrying
`BufferSlice` passed to a command. They cannot inspect opaque root bytes, so buffers referenced only
by a BDA field and textures/samplers referenced only by a heap index must remain alive until
`submit_and_wait()` returns. Releasing such a resource may reuse its descriptor slot.

The shared header maps `CLEAN_GFX_DEVICE_PTR(T)` to `uint64` in C++ and `T*` in Slang. A root POD
can therefore mix typed BDA pointers, `uint32` texture/sampler indices, and 16/32-bit scalar/vector
values. `push_root()` requires a standard-layout, trivially copyable type with four-byte-multiple
size, while shared examples add explicit
`sizeof`, `alignof`, and `offsetof` assertions. Slang is invoked with `-fvk-use-c-layout` when the
compiler exposes it; the explicitly padded triangle root can use scalar layout as an older-compiler
fallback.

For shaders, the preferred path compiles with capability `spvDescriptorHeapEXT` and uses Slang's
`ResourceDescriptorHeap[index]` and `SamplerDescriptorHeap[index]`. This maps to
[`SPV_EXT_descriptor_heap`](https://github.khronos.org/SPIRV-Registry/extensions/EXT/SPV_EXT_descriptor_heap.html),
whose `DescriptorHeapEXT` capability implicitly declares `UntypedPointersKHR`. If `slangc -h` does
not advertise that capability, the backend uses this binding-mapping convention:

- set 0, binding 0: sampled-image array;
- set 0, binding 1: sampler array;
- set 0, binding 2: storage-image array.

The current triangle fallback exercises all three mappings: compute writes through binding 2,
then graphics samples the same image through bindings 0 and 1.

`VkShaderDescriptorSetAndBindingMappingInfoEXT` maps those declarations to constant-offset heap
ranges. This fallback still creates no descriptor-set layout, pool, set, or pipeline layout.

Heap commands and legacy descriptor/push-constant commands mutually invalidate their state. The
backend never records `vkCmdBindDescriptorSets`, descriptor-buffer commands, push descriptors, or
`vkCmdPushConstants`, so a `clean_gfx` command list stays entirely in heap mode. See
[`vkCmdPushDataEXT`](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdPushDataEXT.html)
for the host-copy contract and the descriptor-heaps chapter for the full invalidation list.

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
- Device-address commands remove buffer handles from command arguments, but Vulkan buffers,
  allocations, usage flags, and lifetimes still exist internally. Command recording itself remains
  CPU-driven unless a future device-generated-commands layer is added.
- Vertex data is manually fetched through the typed root pointer, so no vertex-buffer binding is
  needed. Indexed and indirect draws, indirect dispatch, and copy paths use addresses today, but
  the wrapper does not yet expose indirect-count-buffer variants, `vkCmdBindVertexBuffers3KHR`,
  address-range barriers, address-based fill/update, or device-generated commands.
- Textures remain `VkImage` objects with allocation, view, usage, layout transition, and lifetime
  rules. Rendering still uses Vulkan pipeline objects and dynamic-rendering attachment state.
- Descriptor encodings are produced by host calls and placed in host-visible heaps. The current
  allocator does not generate, compact, or relocate descriptor bytes on the GPU.
- The Slang binding-mapping fallback retains set/binding decorations in SPIR-V, although it still
  uses the same heaps and has no Vulkan descriptor-set objects. Native `SPV_EXT_descriptor_heap`
  removes those decorations when the installed Slang compiler supports it.
