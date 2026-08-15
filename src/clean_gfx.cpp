#include <clean_gfx/clean_gfx.hpp>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace clean_gfx
{
namespace
{

constexpr std::uint32_t invalid_heap_index = std::numeric_limits<std::uint32_t>::max();

[[noreturn]] void fail(std::string message)
{
    throw Error(std::move(message));
}

std::string_view result_name(VkResult result)
{
    switch (result)
    {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    default: return "unknown VkResult";
    }
}

void check(VkResult result, std::string_view operation)
{
    if (result == VK_SUCCESS)
        return;

    std::ostringstream stream;
    stream << operation << " failed with " << result_name(result) << " ("
           << static_cast<int>(result) << ')';
    fail(stream.str());
}

template<typename T>
T load_instance_proc(VkInstance instance, const char* name)
{
    const auto proc = vkGetInstanceProcAddr(instance, name);
    if (!proc)
        fail(std::string{"Vulkan loader did not expose required command "} + name);
    return reinterpret_cast<T>(proc);
}

template<typename T>
T load_device_proc(VkDevice device, const char* name)
{
    const auto proc = vkGetDeviceProcAddr(device, name);
    if (!proc)
        fail(std::string{"Vulkan device did not expose required command "} + name);
    return reinterpret_cast<T>(proc);
}

template<typename T>
T align_up(T value, T alignment)
{
    if (alignment == 0)
        return value;
    if (value > std::numeric_limits<T>::max() - (alignment - 1))
        fail("alignment calculation overflowed");
    return ((value + alignment - 1) / alignment) * alignment;
}

template<typename T>
constexpr bool has_flag(T value, T flag)
{
    using U = std::underlying_type_t<T>;
    return (static_cast<U>(value) & static_cast<U>(flag)) != 0;
}

bool has_name(std::span<const VkExtensionProperties> values, const char* name)
{
    return std::ranges::any_of(values, [name](const VkExtensionProperties& value) {
        return std::strcmp(value.extensionName, name) == 0;
    });
}

bool has_name(std::span<const VkLayerProperties> values, const char* name)
{
    return std::ranges::any_of(values, [name](const VkLayerProperties& value) {
        return std::strcmp(value.layerName, name) == 0;
    });
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
    void*)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && callback_data &&
        callback_data->pMessage)
    {
        // Keep the library callback dependency-free. Applications can still install
        // their own messenger; this one makes validation failures debugger-visible.
        std::fputs("clean_gfx validation: ", stderr);
        std::fputs(callback_data->pMessage, stderr);
        std::fputc('\n', stderr);
    }
    return VK_FALSE;
}

VkFormat to_vk(Format format)
{
    switch (format)
    {
    case Format::rgba8_unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::bgra8_unorm: return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::rgba16_float: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::rgba32_float: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::d32_float: return VK_FORMAT_D32_SFLOAT;
    }
    fail("unknown clean_gfx format");
}

std::uint64_t bytes_per_texel(Format format)
{
    switch (format)
    {
    case Format::rgba8_unorm:
    case Format::bgra8_unorm:
    case Format::d32_float: return 4;
    case Format::rgba16_float: return 8;
    case Format::rgba32_float: return 16;
    }
    fail("unknown clean_gfx format");
}

std::uint64_t base_level_byte_size(const TextureDesc& desc)
{
    std::uint64_t size = bytes_per_texel(desc.format);
    for (const auto dimension : {desc.width, desc.height, desc.depth})
    {
        if (dimension != 0 && size > std::numeric_limits<std::uint64_t>::max() / dimension)
            fail("texture byte size exceeds 64 bits");
        size *= dimension;
    }
    return size;
}

std::uint64_t image_resource_byte_size(const TextureDesc& desc)
{
    std::uint64_t total = 0;
    auto width = desc.width;
    auto height = desc.height;
    auto depth = desc.depth;
    for (std::uint32_t mip = 0; mip < desc.mip_levels; ++mip)
    {
        std::uint64_t level_size = bytes_per_texel(desc.format);
        for (const auto dimension : {width, height, depth})
        {
            if (level_size > std::numeric_limits<std::uint64_t>::max() / dimension)
                fail("texture resource size exceeds 64 bits");
            level_size *= dimension;
        }
        if (total > std::numeric_limits<std::uint64_t>::max() - level_size)
            fail("texture resource size exceeds 64 bits");
        total += level_size;
        width = std::max(width / 2, 1u);
        height = std::max(height / 2, 1u);
        depth = std::max(depth / 2, 1u);
    }
    return total;
}

VkFormatFeatureFlags2 required_format_features(TextureUsage usage)
{
    VkFormatFeatureFlags2 result = 0;
    if (has_flag(usage, TextureUsage::sampled))
        result |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
                  VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if (has_flag(usage, TextureUsage::storage))
        result |= VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT |
                  VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT |
                  VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
    if (has_flag(usage, TextureUsage::color_attachment))
        result |= VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT;
    if (has_flag(usage, TextureUsage::depth_attachment))
        result |= VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (has_flag(usage, TextureUsage::transfer_source))
        result |= VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT;
    if (has_flag(usage, TextureUsage::transfer_destination))
        result |= VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
    return result;
}

VkFormatFeatureFlags2 optimal_format_features(VkPhysicalDevice physical_device, Format format)
{
    VkFormatProperties3 properties3{
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3,
    };
    VkFormatProperties2 properties2{
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &properties3,
    };
    vkGetPhysicalDeviceFormatProperties2(physical_device, to_vk(format), &properties2);
    return properties3.optimalTilingFeatures;
}

VkFilter to_vk(Filter filter)
{
    return filter == Filter::linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

VkSamplerAddressMode to_vk(AddressMode mode)
{
    switch (mode)
    {
    case AddressMode::repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case AddressMode::mirrored_repeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case AddressMode::clamp_to_edge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
    fail("unknown sampler address mode");
}

VkPrimitiveTopology to_vk(PrimitiveTopology topology)
{
    switch (topology)
    {
    case PrimitiveTopology::triangle_list: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PrimitiveTopology::triangle_strip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PrimitiveTopology::line_list: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    }
    fail("unknown primitive topology");
}

VkCullModeFlags to_vk(CullMode cull)
{
    switch (cull)
    {
    case CullMode::none: return VK_CULL_MODE_NONE;
    case CullMode::clockwise: return VK_CULL_MODE_BACK_BIT;
    case CullMode::counter_clockwise: return VK_CULL_MODE_BACK_BIT;
    }
    fail("unknown cull mode");
}

VkPipelineStageFlags2 to_vk(Stage stages)
{
    if (stages == Stage::all)
        return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkPipelineStageFlags2 result = 0;
    if (has_flag(stages, Stage::transfer)) result |= VK_PIPELINE_STAGE_2_COPY_BIT;
    if (has_flag(stages, Stage::vertex)) result |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    if (has_flag(stages, Stage::fragment)) result |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    if (has_flag(stages, Stage::compute)) result |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (has_flag(stages, Stage::color_output))
        result |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (has_flag(stages, Stage::host)) result |= VK_PIPELINE_STAGE_2_HOST_BIT;
    if (has_flag(stages, Stage::indirect)) result |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    if (has_flag(stages, Stage::index_input)) result |= VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
    if (has_flag(stages, Stage::depth_tests))
        result |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                  VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    return result;
}

VkAccessFlags2 to_vk(Access accesses)
{
    VkAccessFlags2 result = 0;
    if (has_flag(accesses, Access::transfer_read)) result |= VK_ACCESS_2_TRANSFER_READ_BIT;
    if (has_flag(accesses, Access::transfer_write)) result |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
    if (has_flag(accesses, Access::shader_read))
        result |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    if (has_flag(accesses, Access::shader_write)) result |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    if (has_flag(accesses, Access::color_read))
        result |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
    if (has_flag(accesses, Access::color_write))
        result |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    if (has_flag(accesses, Access::depth_read))
        result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    if (has_flag(accesses, Access::depth_write))
        result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    if (has_flag(accesses, Access::indirect_read)) result |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    if (has_flag(accesses, Access::index_read)) result |= VK_ACCESS_2_INDEX_READ_BIT;
    if (has_flag(accesses, Access::host_read)) result |= VK_ACCESS_2_HOST_READ_BIT;
    if (has_flag(accesses, Access::host_write)) result |= VK_ACCESS_2_HOST_WRITE_BIT;
    if (has_flag(accesses, Access::descriptor_read))
        result |= VK_ACCESS_2_SAMPLER_HEAP_READ_BIT_EXT |
                  VK_ACCESS_2_RESOURCE_HEAP_READ_BIT_EXT;
    return result;
}

void validate_stage_access(Stage stages, Access accesses)
{
    if (accesses == Access::none)
        return;
    if (stages == Stage::none)
        fail("a non-zero access mask requires a non-zero pipeline stage mask");
    if (stages == Stage::all)
        return;

    const auto has_shader_stage = has_flag(stages, Stage::vertex) ||
                                  has_flag(stages, Stage::fragment) ||
                                  has_flag(stages, Stage::compute);
    const auto require = [&](Access access, bool valid, std::string_view description) {
        if (has_flag(accesses, access) && !valid)
            fail(std::string{description} + " access is incompatible with the stage mask");
    };
    const auto transfer_stage = has_flag(stages, Stage::transfer);
    require(Access::transfer_read, transfer_stage, "transfer-read");
    require(Access::transfer_write, transfer_stage, "transfer-write");
    require(Access::shader_read, has_shader_stage, "shader-read");
    require(Access::shader_write, has_shader_stage, "shader-write");
    require(Access::descriptor_read, has_shader_stage, "descriptor-read");
    require(Access::color_read, has_flag(stages, Stage::color_output), "color-read");
    require(Access::color_write, has_flag(stages, Stage::color_output), "color-write");
    require(Access::depth_read, has_flag(stages, Stage::depth_tests), "depth-read");
    require(Access::depth_write, has_flag(stages, Stage::depth_tests), "depth-write");
    require(Access::indirect_read, has_flag(stages, Stage::indirect), "indirect-read");
    require(Access::index_read, has_flag(stages, Stage::index_input), "index-read");
    require(Access::host_read, has_flag(stages, Stage::host), "host-read");
    require(Access::host_write, has_flag(stages, Stage::host), "host-write");
}

struct ImageStateInfo
{
    VkImageLayout layout;
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
};

ImageStateInfo state_info(ImageState state)
{
    switch (state)
    {
    case ImageState::undefined:
        return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0};
    case ImageState::transfer_source:
        return {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT};
    case ImageState::transfer_destination:
        return {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT};
    case ImageState::shader_read:
        return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    case ImageState::storage:
        return {VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    case ImageState::color_attachment:
        return {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
    case ImageState::depth_attachment:
        return {VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    case ImageState::present:
        return {VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0};
    }
    fail("unknown image state");
}

void validate_image_state(TextureUsage usage, ImageState state)
{
    switch (state)
    {
    case ImageState::undefined: return;
    case ImageState::transfer_source:
        if (!has_flag(usage, TextureUsage::transfer_source))
            fail("transfer-source state requires transfer_source texture usage");
        return;
    case ImageState::transfer_destination:
        if (!has_flag(usage, TextureUsage::transfer_destination))
            fail("transfer-destination state requires transfer_destination texture usage");
        return;
    case ImageState::shader_read:
        if (!has_flag(usage, TextureUsage::sampled))
            fail("shader-read state requires sampled texture usage");
        return;
    case ImageState::storage:
        if (!has_flag(usage, TextureUsage::storage))
            fail("storage state requires storage texture usage");
        return;
    case ImageState::color_attachment:
        if (!has_flag(usage, TextureUsage::color_attachment))
            fail("color-attachment state requires color_attachment texture usage");
        return;
    case ImageState::depth_attachment:
        if (!has_flag(usage, TextureUsage::depth_attachment))
            fail("depth-attachment state requires depth_attachment texture usage");
        return;
    case ImageState::present:
        fail("present state is unavailable because clean_gfx does not expose swapchains");
    }
    fail("unknown image state");
}

VkBufferUsageFlags universal_buffer_usage()
{
    return VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
           VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
           VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
}

constexpr VkAddressCommandFlagsKHR address_flags =
    VK_ADDRESS_COMMAND_FULLY_BOUND_BIT_KHR |
    VK_ADDRESS_COMMAND_STORAGE_BUFFER_USAGE_BIT_KHR;

} // namespace

namespace detail
{

struct DeviceFunctions
{
    PFN_vkWriteSamplerDescriptorsEXT write_sampler_descriptors = nullptr;
    PFN_vkWriteResourceDescriptorsEXT write_resource_descriptors = nullptr;
    PFN_vkCmdBindSamplerHeapEXT cmd_bind_sampler_heap = nullptr;
    PFN_vkCmdBindResourceHeapEXT cmd_bind_resource_heap = nullptr;
    PFN_vkCmdPushDataEXT cmd_push_data = nullptr;
    PFN_vkCmdBindIndexBuffer3KHR cmd_bind_index_buffer = nullptr;
    PFN_vkCmdDrawIndirect2KHR cmd_draw_indirect = nullptr;
    PFN_vkCmdDrawIndexedIndirect2KHR cmd_draw_indexed_indirect = nullptr;
    PFN_vkCmdDispatchIndirect2KHR cmd_dispatch_indirect = nullptr;
    PFN_vkCmdCopyMemoryKHR cmd_copy_memory = nullptr;
    PFN_vkCmdCopyMemoryToImageKHR cmd_copy_memory_to_image = nullptr;
    PFN_vkCmdCopyImageToMemoryKHR cmd_copy_image_to_memory = nullptr;
};

struct BackingBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceAddress address = 0;
    VkDeviceSize size = 0;
    VkDeviceSize allocation_size = 0;
    bool coherent = false;
};

struct DescriptorHeap
{
    BackingBuffer backing;
    VkDeviceSize bind_offset = 0;
    VkDeviceSize bind_size = 0;
    VkDeviceSize reserved_size = 0;
    VkDeviceSize descriptor_size = 0;
    std::uint32_t first_index = 0;
    std::uint32_t capacity = 0;
    std::uint32_t next_slot = 0;
    std::vector<std::uint32_t> free_slots;
    std::mutex mutex;

    [[nodiscard]] std::uint32_t allocate()
    {
        const std::scoped_lock lock(mutex);
        std::uint32_t slot = 0;
        if (!free_slots.empty())
        {
            slot = free_slots.back();
            free_slots.pop_back();
        }
        else
        {
            if (next_slot == capacity)
                fail("descriptor heap capacity exhausted");
            slot = next_slot++;
        }
        if (slot > std::numeric_limits<std::uint32_t>::max() - first_index)
            fail("descriptor heap index exceeds 32 bits");
        return first_index + slot;
    }

    void release(std::uint32_t index)
    {
        if (index == invalid_heap_index)
            return;
        const std::scoped_lock lock(mutex);
        if (index < first_index || index >= first_index + capacity)
            return;
        free_slots.push_back(index - first_index);
    }

    [[nodiscard]] void* host_address(std::uint32_t index) const
    {
        const auto offset = static_cast<VkDeviceSize>(index) * descriptor_size;
        return static_cast<std::byte*>(backing.mapped) + bind_offset + offset;
    }

    [[nodiscard]] VkBindHeapInfoEXT bind_info() const
    {
        return {
            .sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
            .pNext = nullptr,
            .heapRange = {backing.address + bind_offset, bind_size},
            .reservedRangeOffset = 0,
            .reservedRangeSize = reserved_size,
        };
    }
};

struct DeviceState
{
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug_messenger = nullptr;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkPhysicalDeviceProperties physical_properties{};
    VkPhysicalDeviceVulkan13Properties vulkan13_properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
    VkPhysicalDeviceDescriptorHeapPropertiesEXT heap_properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT};
    DeviceFunctions fn;
    DescriptorHeap resource_heap;
    DescriptorHeap sampler_heap;
    DeviceCaps caps;
    mutable std::atomic_uint32_t live_memory_allocations{0};
    mutable std::mutex queue_mutex;

    ~DeviceState()
    {
        if (device)
            vkDeviceWaitIdle(device);
        destroy_backing(resource_heap.backing);
        destroy_backing(sampler_heap.backing);
        if (device)
            vkDestroyDevice(device, nullptr);
        if (instance && debug_messenger && destroy_debug_messenger)
            destroy_debug_messenger(instance, debug_messenger, nullptr);
        if (instance)
            vkDestroyInstance(instance, nullptr);
    }

    DeviceState() = default;
    DeviceState(const DeviceState&) = delete;
    DeviceState& operator=(const DeviceState&) = delete;

    void destroy_backing(BackingBuffer& backing) const noexcept
    {
        if (!device)
            return;
        if (backing.mapped)
            vkUnmapMemory(device, backing.memory);
        if (backing.buffer)
            vkDestroyBuffer(device, backing.buffer, nullptr);
        if (backing.memory)
            free_memory(backing.memory);
        backing = {};
    }

    [[nodiscard]] VkDeviceMemory allocate_memory(const VkMemoryAllocateInfo& info) const
    {
        if (info.memoryTypeIndex >= memory_properties.memoryTypeCount)
            fail("internal Vulkan memory type index is out of range");
        const auto heap_index = memory_properties.memoryTypes[info.memoryTypeIndex].heapIndex;
        if (heap_index >= memory_properties.memoryHeapCount ||
            info.allocationSize > memory_properties.memoryHeaps[heap_index].size)
        {
            fail("Vulkan allocation exceeds the selected memory heap size");
        }

        const auto maximum = physical_properties.limits.maxMemoryAllocationCount;
        auto count = live_memory_allocations.load(std::memory_order_relaxed);
        for (;;)
        {
            if (count >= maximum)
                fail("Vulkan maxMemoryAllocationCount exhausted");
            if (live_memory_allocations.compare_exchange_weak(
                    count, count + 1, std::memory_order_acq_rel, std::memory_order_relaxed))
            {
                break;
            }
        }

        VkDeviceMemory memory = VK_NULL_HANDLE;
        const auto result = vkAllocateMemory(device, &info, nullptr, &memory);
        if (result != VK_SUCCESS)
        {
            live_memory_allocations.fetch_sub(1, std::memory_order_acq_rel);
            check(result, "vkAllocateMemory");
        }
        return memory;
    }

    void free_memory(VkDeviceMemory memory) const noexcept
    {
        if (!memory)
            return;
        vkFreeMemory(device, memory, nullptr);
        live_memory_allocations.fetch_sub(1, std::memory_order_acq_rel);
    }

    [[nodiscard]] std::uint32_t find_memory_type(std::uint32_t bits,
                                                  VkMemoryPropertyFlags required,
                                                  VkMemoryPropertyFlags preferred) const
    {
        std::optional<std::uint32_t> best;
        std::uint32_t best_score = 0;
        for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
        {
            if ((bits & (1u << i)) == 0)
                continue;
            const auto flags = memory_properties.memoryTypes[i].propertyFlags;
            if ((flags & required) != required)
                continue;
            constexpr auto forbidden = VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT |
                                       VK_MEMORY_PROPERTY_PROTECTED_BIT |
                                       VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD |
                                       VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD;
            if ((flags & forbidden) != 0)
                continue;
            const auto heap_index = memory_properties.memoryTypes[i].heapIndex;
            if (heap_index >= memory_properties.memoryHeapCount ||
                (memory_properties.memoryHeaps[heap_index].flags &
                 VK_MEMORY_HEAP_TILE_MEMORY_BIT_QCOM) != 0)
            {
                continue;
            }
            const auto score = static_cast<std::uint32_t>(std::popcount(flags & preferred));
            if (!best || score > best_score)
            {
                best = i;
                best_score = score;
            }
        }
        if (!best)
            fail("no Vulkan memory type satisfies the requested allocation");
        return *best;
    }

    [[nodiscard]] BackingBuffer create_backing_buffer(
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VkMemoryPropertyFlags required,
        VkMemoryPropertyFlags preferred) const
    {
        if (size == 0 || size > vulkan13_properties.maxBufferSize)
            fail("buffer size exceeds VkPhysicalDeviceVulkan13Properties::maxBufferSize");
        BackingBuffer result;
        result.size = size;
        try
        {
            const VkBufferCreateInfo buffer_info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .size = size,
                .usage = usage,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
            };
            check(vkCreateBuffer(device, &buffer_info, nullptr, &result.buffer), "vkCreateBuffer");

            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(device, result.buffer, &requirements);
            result.allocation_size = requirements.size;
            const auto memory_type = find_memory_type(requirements.memoryTypeBits, required, preferred);
            const auto memory_flags = memory_properties.memoryTypes[memory_type].propertyFlags;
            result.coherent = (memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

            const VkMemoryDedicatedAllocateInfo dedicated_info{
                .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                .pNext = nullptr,
                .image = VK_NULL_HANDLE,
                .buffer = result.buffer,
            };
            const VkMemoryAllocateFlagsInfo flags_info{
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                .pNext = &dedicated_info,
                .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
                .deviceMask = 0,
            };
            const VkMemoryAllocateInfo allocate_info{
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .pNext = &flags_info,
                .allocationSize = requirements.size,
                .memoryTypeIndex = memory_type,
            };
            result.memory = allocate_memory(allocate_info);
            check(vkBindBufferMemory(device, result.buffer, result.memory, 0),
                  "vkBindBufferMemory");

            if ((memory_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
            {
                check(vkMapMemory(device, result.memory, 0, VK_WHOLE_SIZE, 0, &result.mapped),
                      "vkMapMemory");
            }

            const VkBufferDeviceAddressInfo address_info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                .pNext = nullptr,
                .buffer = result.buffer,
            };
            result.address = vkGetBufferDeviceAddress(device, &address_info);
            if (result.address == 0)
                fail("vkGetBufferDeviceAddress returned zero");
            return result;
        }
        catch (...)
        {
            destroy_backing(result);
            throw;
        }
    }

    void flush(const BackingBuffer& backing) const
    {
        if (!backing.mapped || backing.coherent)
            return;
        const VkMappedMemoryRange range{
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .pNext = nullptr,
            .memory = backing.memory,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        check(vkFlushMappedMemoryRanges(device, 1, &range), "vkFlushMappedMemoryRanges");
    }

    void invalidate(const BackingBuffer& backing) const
    {
        if (!backing.mapped || backing.coherent)
            return;
        const VkMappedMemoryRange range{
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .pNext = nullptr,
            .memory = backing.memory,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        check(vkInvalidateMappedMemoryRanges(device, 1, &range),
              "vkInvalidateMappedMemoryRanges");
    }

    void initialize_heap(DescriptorHeap& heap,
                         std::uint32_t capacity,
                         VkDeviceSize descriptor_size,
                         VkDeviceSize descriptor_alignment,
                         VkDeviceSize heap_alignment,
                         VkDeviceSize reserved_size,
                         VkDeviceSize maximum_size)
    {
        if (capacity == 0)
            fail("descriptor heap capacity must be non-zero");
        if (descriptor_size == 0)
            fail("Vulkan reported a zero descriptor size");
        if (descriptor_size % std::max<VkDeviceSize>(descriptor_alignment, 1) != 0)
            fail("Vulkan descriptor size does not satisfy its advertised alignment");

        const auto offset_alignment = descriptor_size;
        const auto user_offset = align_up(reserved_size, offset_alignment);
        if (user_offset > maximum_size ||
            static_cast<VkDeviceSize>(capacity) > (maximum_size - user_offset) / descriptor_size)
            fail("requested descriptor heap exceeds the device limit");
        const auto desired_size =
            user_offset + static_cast<VkDeviceSize>(capacity) * descriptor_size;

        const auto address_alignment = std::max<VkDeviceSize>(heap_alignment, 1);
        if (desired_size > std::numeric_limits<VkDeviceSize>::max() - (address_alignment - 1))
            fail("descriptor heap backing size exceeds 64 bits");
        heap.backing = create_backing_buffer(
            desired_size + address_alignment - 1,
            VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        const auto aligned_address = align_up(heap.backing.address, address_alignment);
        heap.bind_offset = aligned_address - heap.backing.address;
        heap.bind_size = desired_size;
        heap.reserved_size = reserved_size;
        heap.descriptor_size = descriptor_size;
        const auto first_index = user_offset / descriptor_size;
        if (first_index > std::numeric_limits<std::uint32_t>::max())
            fail("descriptor heap reserved range produces an index wider than 32 bits");
        heap.first_index = static_cast<std::uint32_t>(first_index);
        heap.capacity = capacity;
        heap.next_slot = 0;

        if (heap.bind_offset > heap.backing.size ||
            heap.bind_size > heap.backing.size - heap.bind_offset)
            fail("internal descriptor heap alignment calculation overflowed the buffer");
        if (heap.first_index > std::numeric_limits<std::uint32_t>::max() - capacity)
            fail("descriptor heap indices exceed 32 bits");
    }
};

struct BufferAllocation
{
    std::shared_ptr<DeviceState> state;
    BackingBuffer backing;

    ~BufferAllocation()
    {
        if (state)
            state->destroy_backing(backing);
    }
};

} // namespace detail

struct Buffer::Impl
{
    std::shared_ptr<detail::BufferAllocation> allocation;
};

struct Texture::Impl
{
    std::shared_ptr<detail::DeviceState> state;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    TextureDesc desc;
    std::uint32_t sampled_index = invalid_heap_index;
    std::uint32_t storage_index = invalid_heap_index;

    ~Impl()
    {
        if (!state)
            return;
        state->resource_heap.release(sampled_index);
        state->resource_heap.release(storage_index);
        if (view)
            vkDestroyImageView(state->device, view, nullptr);
        if (image)
            vkDestroyImage(state->device, image, nullptr);
        if (memory)
            state->free_memory(memory);
    }
};

struct Sampler::Impl
{
    std::shared_ptr<detail::DeviceState> state;
    std::uint32_t index = invalid_heap_index;

    ~Impl()
    {
        if (state)
            state->sampler_heap.release(index);
    }
};

struct Pipeline::Impl
{
    std::shared_ptr<detail::DeviceState> state;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
    Format color_format = Format::rgba8_unorm;
    Format depth_format = Format::d32_float;
    bool depth_enabled = false;

    ~Impl()
    {
        if (state && pipeline)
            vkDestroyPipeline(state->device, pipeline, nullptr);
    }
};

struct CommandList::Impl
{
    std::shared_ptr<detail::DeviceState> state;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    bool recording = true;
    bool rendering = false;
    Format rendering_color_format = Format::rgba8_unorm;
    Format rendering_depth_format = Format::d32_float;
    bool rendering_has_depth = false;
    std::shared_ptr<Pipeline::Impl> bound_graphics;
    std::shared_ptr<Pipeline::Impl> bound_compute;
    std::vector<std::shared_ptr<void>> retained_resources;

    ~Impl()
    {
        if (!state || !command_pool)
            return;
        vkDestroyCommandPool(state->device, command_pool, nullptr);
    }
};

BufferSlice BufferSlice::subspan(std::uint64_t offset, std::uint64_t byte_count) const
{
    if (offset > size_)
        fail("buffer slice offset is out of range");
    const auto remaining = size_ - offset;
    if (byte_count == 0)
        byte_count = remaining;
    if (byte_count > remaining)
        fail("buffer slice size is out of range");
    if (address_ > std::numeric_limits<std::uint64_t>::max() - offset)
        fail("buffer slice address overflow");
    return BufferSlice{address_ + offset, byte_count, allocation_};
}

BufferSlice::BufferSlice(DeviceAddress address,
                         std::uint64_t size,
                         std::shared_ptr<detail::BufferAllocation> allocation) noexcept
    : address_(address), size_(size), allocation_(std::move(allocation))
{}

namespace
{

std::vector<VkExtensionProperties> enumerate_device_extensions(VkPhysicalDevice physical_device)
{
    for (;;)
    {
        std::uint32_t count = 0;
        check(vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, nullptr),
              "vkEnumerateDeviceExtensionProperties");
        std::vector<VkExtensionProperties> values(count);
        const auto result = vkEnumerateDeviceExtensionProperties(
            physical_device, nullptr, &count, values.data());
        if (result == VK_INCOMPLETE)
            continue;
        check(result, "vkEnumerateDeviceExtensionProperties");
        values.resize(count);
        return values;
    }
}

std::vector<VkExtensionProperties> enumerate_instance_extensions()
{
    for (;;)
    {
        std::uint32_t count = 0;
        check(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr),
              "vkEnumerateInstanceExtensionProperties");
        std::vector<VkExtensionProperties> values(count);
        const auto result =
            vkEnumerateInstanceExtensionProperties(nullptr, &count, values.data());
        if (result == VK_INCOMPLETE)
            continue;
        check(result, "vkEnumerateInstanceExtensionProperties");
        values.resize(count);
        return values;
    }
}

std::vector<VkLayerProperties> enumerate_instance_layers()
{
    for (;;)
    {
        std::uint32_t count = 0;
        check(vkEnumerateInstanceLayerProperties(&count, nullptr),
              "vkEnumerateInstanceLayerProperties");
        std::vector<VkLayerProperties> values(count);
        const auto result = vkEnumerateInstanceLayerProperties(&count, values.data());
        if (result == VK_INCOMPLETE)
            continue;
        check(result, "vkEnumerateInstanceLayerProperties");
        values.resize(count);
        return values;
    }
}

struct QueriedFeatures
{
    VkPhysicalDeviceFeatures2 core{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features vulkan11{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features vulkan12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features vulkan13{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan14Features vulkan14{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    VkPhysicalDeviceDescriptorHeapFeaturesEXT descriptor_heap{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
    VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR address_commands{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_ADDRESS_COMMANDS_FEATURES_KHR};
    VkPhysicalDeviceShaderUntypedPointersFeaturesKHR untyped_pointers{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR};

    QueriedFeatures()
    {
        core.pNext = &vulkan11;
        vulkan11.pNext = &vulkan12;
        vulkan12.pNext = &vulkan13;
        vulkan13.pNext = &vulkan14;
        vulkan14.pNext = &descriptor_heap;
        descriptor_heap.pNext = &address_commands;
        address_commands.pNext = &untyped_pointers;
    }
};

struct Candidate
{
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceDescriptorHeapPropertiesEXT heap_properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT};
    VkPhysicalDeviceVulkan13Properties vulkan13_properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
};

std::optional<Candidate> inspect_candidate(VkPhysicalDevice physical_device,
                                           std::string& rejection)
{
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_device, &properties);
    const std::string prefix = std::string{properties.deviceName} + ": ";

    const auto extensions = enumerate_device_extensions(physical_device);
    constexpr std::array required_extensions{
        VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME,
        VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME,
        VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME,
    };
    std::vector<std::string_view> missing_extensions;
    for (const char* name : required_extensions)
    {
        if (!has_name(extensions, name))
            missing_extensions.emplace_back(name);
    }
    if (!missing_extensions.empty())
    {
        std::ostringstream stream;
        stream << prefix << "missing device extension";
        for (const auto name : missing_extensions)
            stream << ' ' << name;
        rejection = stream.str();
        return std::nullopt;
    }
    if (properties.apiVersion < VK_API_VERSION_1_4)
    {
        std::ostringstream stream;
        stream << prefix << "reports Vulkan " << VK_API_VERSION_MAJOR(properties.apiVersion)
               << '.' << VK_API_VERSION_MINOR(properties.apiVersion)
               << "; Vulkan 1.4 is required";
        rejection = stream.str();
        return std::nullopt;
    }

    QueriedFeatures features;
    vkGetPhysicalDeviceFeatures2(physical_device, &features.core);
    std::vector<std::string_view> missing_features;
    const auto require = [&](VkBool32 supported, std::string_view name) {
        if (supported != VK_TRUE)
            missing_features.push_back(name);
    };
    require(features.core.features.shaderInt16, "shaderInt16");
    require(features.core.features.fragmentStoresAndAtomics, "fragmentStoresAndAtomics");
    require(features.core.features.vertexPipelineStoresAndAtomics,
            "vertexPipelineStoresAndAtomics");
    require(features.core.features.shaderStorageImageReadWithoutFormat,
            "shaderStorageImageReadWithoutFormat");
    require(features.core.features.shaderStorageImageWriteWithoutFormat,
            "shaderStorageImageWriteWithoutFormat");
    require(features.core.features.multiDrawIndirect, "multiDrawIndirect");
    require(features.core.features.drawIndirectFirstInstance, "drawIndirectFirstInstance");
    require(features.vulkan11.storageBuffer16BitAccess, "storageBuffer16BitAccess");
    require(features.vulkan11.storagePushConstant16, "storagePushConstant16");
    require(features.vulkan11.shaderDrawParameters, "shaderDrawParameters");
    require(features.vulkan12.shaderFloat16, "shaderFloat16");
    require(features.vulkan12.runtimeDescriptorArray, "runtimeDescriptorArray");
    require(features.vulkan12.scalarBlockLayout, "scalarBlockLayout");
    require(features.vulkan12.bufferDeviceAddress, "bufferDeviceAddress");
    require(features.vulkan13.synchronization2, "synchronization2");
    require(features.vulkan13.dynamicRendering, "dynamicRendering");
    require(features.vulkan14.maintenance5, "maintenance5");
    require(features.descriptor_heap.descriptorHeap, "descriptorHeap");
    require(features.address_commands.deviceAddressCommands, "deviceAddressCommands");
    require(features.untyped_pointers.shaderUntypedPointers, "shaderUntypedPointers");
    if (!missing_features.empty())
    {
        std::ostringstream stream;
        stream << prefix << "missing feature";
        for (const auto name : missing_features)
            stream << ' ' << name;
        rejection = stream.str();
        return std::nullopt;
    }

    std::uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_count, queues.data());
    const auto queue = std::ranges::find_if(queues, [](const VkQueueFamilyProperties& value) {
        constexpr auto required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        return value.queueCount != 0 && (value.queueFlags & required) == required;
    });
    if (queue == queues.end())
    {
        rejection = prefix + "no queue family supporting both graphics and compute";
        return std::nullopt;
    }

    Candidate result;
    result.physical_device = physical_device;
    result.queue_family = static_cast<std::uint32_t>(std::distance(queues.begin(), queue));
    result.properties = properties;

    result.heap_properties.pNext = &result.vulkan13_properties;
    VkPhysicalDeviceProperties2 properties2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &result.heap_properties,
    };
    vkGetPhysicalDeviceProperties2(physical_device, &properties2);
    result.heap_properties.pNext = nullptr;
    rejection.clear();
    return result;
}

VkShaderModule create_shader_module(VkDevice device, std::span<const std::uint32_t> words)
{
    if (words.empty())
        fail("SPIR-V shader bytecode is empty");
    const VkShaderModuleCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = words.size_bytes(),
        .pCode = words.data(),
    };
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule");
    return module;
}

std::array<VkDescriptorSetAndBindingMappingEXT, 3> make_standard_mappings(
    const detail::DeviceState& state)
{
    VkDescriptorMappingSourceDataEXT texture_data{};
    texture_data.constantOffset = {
        .heapOffset = 0,
        .heapArrayStride = static_cast<std::uint32_t>(state.resource_heap.descriptor_size),
        .pEmbeddedSampler = nullptr,
        .samplerHeapOffset = 0,
        .samplerHeapArrayStride = 0,
    };
    VkDescriptorMappingSourceDataEXT sampler_data{};
    sampler_data.constantOffset = {
        .heapOffset = 0,
        .heapArrayStride = static_cast<std::uint32_t>(state.sampler_heap.descriptor_size),
        .pEmbeddedSampler = nullptr,
        .samplerHeapOffset = 0,
        .samplerHeapArrayStride = 0,
    };

    return {{
        {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT,
            .pNext = nullptr,
            .descriptorSet = 0,
            .firstBinding = 0,
            .bindingCount = 1,
            .resourceMask = VK_SPIRV_RESOURCE_TYPE_SAMPLED_IMAGE_BIT_EXT,
            .source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT,
            .sourceData = texture_data,
        },
        {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT,
            .pNext = nullptr,
            .descriptorSet = 0,
            .firstBinding = 1,
            .bindingCount = 1,
            .resourceMask = VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT,
            .source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT,
            .sourceData = sampler_data,
        },
        {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT,
            .pNext = nullptr,
            .descriptorSet = 0,
            .firstBinding = 2,
            .bindingCount = 1,
            .resourceMask = VK_SPIRV_RESOURCE_TYPE_READ_WRITE_IMAGE_BIT_EXT,
            .source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT,
            .sourceData = texture_data,
        },
    }};
}

} // namespace

Device Device::create(const DeviceDesc& desc)
{
    static_assert(sizeof(void*) == 8, "clean_gfx requires a 64-bit host ABI");

    std::uint32_t loader_version = VK_API_VERSION_1_0;
    check(vkEnumerateInstanceVersion(&loader_version), "vkEnumerateInstanceVersion");
    if (loader_version < VK_API_VERSION_1_4)
        fail("clean_gfx requires a Vulkan 1.4 loader");

    auto state = std::make_shared<detail::DeviceState>();
    const auto instance_extensions = enumerate_instance_extensions();
    const auto layers = enumerate_instance_layers();
    const bool debug_utils_available =
        has_name(instance_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const bool validation_available =
        has_name(layers, "VK_LAYER_KHRONOS_validation");

    std::vector<const char*> enabled_instance_extensions;
    std::vector<const char*> enabled_layers;
    if (desc.enable_validation && debug_utils_available)
        enabled_instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (desc.enable_validation && validation_available)
        enabled_layers.push_back("VK_LAYER_KHRONOS_validation");

    const VkApplicationInfo app_info{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = nullptr,
        .pApplicationName = desc.application_name.c_str(),
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .pEngineName = "clean_gfx",
        .engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .apiVersion = VK_API_VERSION_1_4,
    };
    const VkInstanceCreateInfo instance_info{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .pApplicationInfo = &app_info,
        .enabledLayerCount = static_cast<std::uint32_t>(enabled_layers.size()),
        .ppEnabledLayerNames = enabled_layers.data(),
        .enabledExtensionCount = static_cast<std::uint32_t>(enabled_instance_extensions.size()),
        .ppEnabledExtensionNames = enabled_instance_extensions.data(),
    };
    check(vkCreateInstance(&instance_info, nullptr, &state->instance), "vkCreateInstance");

    if (desc.enable_validation && debug_utils_available)
    {
        const auto create_debug = load_instance_proc<PFN_vkCreateDebugUtilsMessengerEXT>(
            state->instance, "vkCreateDebugUtilsMessengerEXT");
        state->destroy_debug_messenger =
            load_instance_proc<PFN_vkDestroyDebugUtilsMessengerEXT>(
                state->instance, "vkDestroyDebugUtilsMessengerEXT");
        const VkDebugUtilsMessengerCreateInfoEXT debug_info{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .pNext = nullptr,
            .flags = 0,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_callback,
            .pUserData = nullptr,
        };
        check(create_debug(state->instance, &debug_info, nullptr, &state->debug_messenger),
              "vkCreateDebugUtilsMessengerEXT");
    }

    std::vector<VkPhysicalDevice> physical_devices;
    for (;;)
    {
        std::uint32_t physical_device_count = 0;
        check(vkEnumeratePhysicalDevices(state->instance, &physical_device_count, nullptr),
              "vkEnumeratePhysicalDevices");
        physical_devices.resize(physical_device_count);
        const auto result = vkEnumeratePhysicalDevices(
            state->instance, &physical_device_count, physical_devices.data());
        if (result == VK_INCOMPLETE)
            continue;
        check(result, "vkEnumeratePhysicalDevices");
        physical_devices.resize(physical_device_count);
        break;
    }

    std::optional<Candidate> selected;
    std::vector<std::string> rejection_reasons;
    for (const auto physical_device : physical_devices)
    {
        std::string rejection;
        auto candidate = inspect_candidate(physical_device, rejection);
        if (!candidate)
        {
            rejection_reasons.push_back(std::move(rejection));
            continue;
        }
        if (!selected || candidate->properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            selected = *candidate;
        if (candidate->properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            break;
    }
    if (!selected)
    {
        std::ostringstream stream;
        stream << "no compatible Vulkan device found";
        if (physical_devices.empty())
            stream << ": vkEnumeratePhysicalDevices returned no devices";
        for (const auto& reason : rejection_reasons)
            stream << "\n  - " << reason;
        fail(stream.str());
    }

    state->physical_device = selected->physical_device;
    state->queue_family = selected->queue_family;
    state->physical_properties = selected->properties;
    state->heap_properties = selected->heap_properties;
    state->vulkan13_properties = selected->vulkan13_properties;
    state->vulkan13_properties.pNext = nullptr;
    vkGetPhysicalDeviceMemoryProperties(state->physical_device, &state->memory_properties);

    QueriedFeatures enabled_features;
    vkGetPhysicalDeviceFeatures2(state->physical_device, &enabled_features.core);
    enabled_features.core.features = {};
    enabled_features.core.features.shaderInt16 = VK_TRUE;
    enabled_features.core.features.fragmentStoresAndAtomics = VK_TRUE;
    enabled_features.core.features.vertexPipelineStoresAndAtomics = VK_TRUE;
    enabled_features.core.features.shaderStorageImageReadWithoutFormat = VK_TRUE;
    enabled_features.core.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    enabled_features.core.features.multiDrawIndirect = VK_TRUE;
    enabled_features.core.features.drawIndirectFirstInstance = VK_TRUE;
    enabled_features.vulkan11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext = &enabled_features.vulkan12,
        .storageBuffer16BitAccess = VK_TRUE,
        .uniformAndStorageBuffer16BitAccess = VK_FALSE,
        .storagePushConstant16 = VK_TRUE,
        .shaderDrawParameters = VK_TRUE,
    };
    enabled_features.vulkan12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &enabled_features.vulkan13,
        .shaderFloat16 = VK_TRUE,
        .runtimeDescriptorArray = VK_TRUE,
        .scalarBlockLayout = VK_TRUE,
        .bufferDeviceAddress = VK_TRUE,
    };
    enabled_features.vulkan13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &enabled_features.vulkan14,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    enabled_features.vulkan14 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
        .pNext = &enabled_features.descriptor_heap,
        .maintenance5 = VK_TRUE,
    };
    enabled_features.descriptor_heap = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT,
        .pNext = &enabled_features.address_commands,
        .descriptorHeap = VK_TRUE,
        .descriptorHeapCaptureReplay = VK_FALSE,
    };
    enabled_features.address_commands = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_ADDRESS_COMMANDS_FEATURES_KHR,
        .pNext = &enabled_features.untyped_pointers,
        .deviceAddressCommands = VK_TRUE,
    };
    enabled_features.untyped_pointers = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR,
        .pNext = nullptr,
        .shaderUntypedPointers = VK_TRUE,
    };

    constexpr float queue_priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queueFamilyIndex = state->queue_family,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };
    constexpr std::array enabled_device_extensions{
        VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME,
        VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME,
        VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME,
    };
    const VkDeviceCreateInfo device_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &enabled_features.core,
        .flags = 0,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = static_cast<std::uint32_t>(enabled_device_extensions.size()),
        .ppEnabledExtensionNames = enabled_device_extensions.data(),
        .pEnabledFeatures = nullptr,
    };
    check(vkCreateDevice(state->physical_device, &device_info, nullptr, &state->device),
          "vkCreateDevice");
    vkGetDeviceQueue(state->device, state->queue_family, 0, &state->queue);

    state->fn.write_sampler_descriptors =
        load_device_proc<PFN_vkWriteSamplerDescriptorsEXT>(
            state->device, "vkWriteSamplerDescriptorsEXT");
    state->fn.write_resource_descriptors =
        load_device_proc<PFN_vkWriteResourceDescriptorsEXT>(
            state->device, "vkWriteResourceDescriptorsEXT");
    state->fn.cmd_bind_sampler_heap = load_device_proc<PFN_vkCmdBindSamplerHeapEXT>(
        state->device, "vkCmdBindSamplerHeapEXT");
    state->fn.cmd_bind_resource_heap = load_device_proc<PFN_vkCmdBindResourceHeapEXT>(
        state->device, "vkCmdBindResourceHeapEXT");
    state->fn.cmd_push_data = load_device_proc<PFN_vkCmdPushDataEXT>(
        state->device, "vkCmdPushDataEXT");
    state->fn.cmd_bind_index_buffer = load_device_proc<PFN_vkCmdBindIndexBuffer3KHR>(
        state->device, "vkCmdBindIndexBuffer3KHR");
    state->fn.cmd_draw_indirect = load_device_proc<PFN_vkCmdDrawIndirect2KHR>(
        state->device, "vkCmdDrawIndirect2KHR");
    state->fn.cmd_draw_indexed_indirect =
        load_device_proc<PFN_vkCmdDrawIndexedIndirect2KHR>(
            state->device, "vkCmdDrawIndexedIndirect2KHR");
    state->fn.cmd_dispatch_indirect = load_device_proc<PFN_vkCmdDispatchIndirect2KHR>(
        state->device, "vkCmdDispatchIndirect2KHR");
    state->fn.cmd_copy_memory = load_device_proc<PFN_vkCmdCopyMemoryKHR>(
        state->device, "vkCmdCopyMemoryKHR");
    state->fn.cmd_copy_memory_to_image = load_device_proc<PFN_vkCmdCopyMemoryToImageKHR>(
        state->device, "vkCmdCopyMemoryToImageKHR");
    state->fn.cmd_copy_image_to_memory = load_device_proc<PFN_vkCmdCopyImageToMemoryKHR>(
        state->device, "vkCmdCopyImageToMemoryKHR");

    state->initialize_heap(
        state->resource_heap,
        desc.texture_capacity,
        state->heap_properties.imageDescriptorSize,
        state->heap_properties.imageDescriptorAlignment,
        state->heap_properties.resourceHeapAlignment,
        state->heap_properties.minResourceHeapReservedRange,
        state->heap_properties.maxResourceHeapSize);
    state->initialize_heap(
        state->sampler_heap,
        desc.sampler_capacity,
        state->heap_properties.samplerDescriptorSize,
        state->heap_properties.samplerDescriptorAlignment,
        state->heap_properties.samplerHeapAlignment,
        state->heap_properties.minSamplerHeapReservedRange,
        state->heap_properties.maxSamplerHeapSize);

    state->caps = {
        .device_name = state->physical_properties.deviceName,
        .api_version = state->physical_properties.apiVersion,
        .max_push_data_size = static_cast<std::uint32_t>(std::min<VkDeviceSize>(
            state->heap_properties.maxPushDataSize,
            std::numeric_limits<std::uint32_t>::max())),
        .image_descriptor_size = state->heap_properties.imageDescriptorSize,
        .sampler_descriptor_size = state->heap_properties.samplerDescriptorSize,
        .first_texture_index = state->resource_heap.first_index,
        .first_sampler_index = state->sampler_heap.first_index,
    };
    return Device{std::move(state)};
}

Buffer Device::create_buffer(const BufferDesc& desc) const
{
    if (!impl_)
        fail("create_buffer called on an empty device");
    if (desc.size == 0)
        fail("buffer size must be non-zero");

    VkMemoryPropertyFlags required = 0;
    VkMemoryPropertyFlags preferred = 0;
    switch (desc.memory)
    {
    case MemoryType::upload:
        required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        preferred = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        break;
    case MemoryType::gpu:
        required = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        break;
    case MemoryType::readback:
        required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        preferred = VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        break;
    }

    auto allocation = std::make_shared<detail::BufferAllocation>();
    allocation->state = impl_;
    allocation->backing = impl_->create_backing_buffer(
        desc.size, universal_buffer_usage(), required, preferred);
    auto result = std::make_unique<Buffer::Impl>();
    result->allocation = std::move(allocation);
    return Buffer{std::move(result)};
}

Texture Device::create_texture(const TextureDesc& desc) const
{
    if (!impl_)
        fail("create_texture called on an empty device");
    if (desc.width == 0 || desc.height == 0 || desc.depth == 0 || desc.mip_levels == 0)
        fail("texture dimensions and mip count must be non-zero");
    const auto maximum_mip_levels = static_cast<std::uint32_t>(
        std::bit_width(std::max({desc.width, desc.height, desc.depth})));
    if (desc.mip_levels > maximum_mip_levels)
        fail("texture mip count exceeds the maximum for its dimensions");
    const bool depth_format = desc.format == Format::d32_float;
    if (depth_format && has_flag(desc.usage, TextureUsage::color_attachment))
        fail("a depth format cannot be used as a color attachment");
    if (!depth_format && has_flag(desc.usage, TextureUsage::depth_attachment))
        fail("a color format cannot be used as a depth attachment");
    if ((has_flag(desc.usage, TextureUsage::color_attachment) ||
         has_flag(desc.usage, TextureUsage::depth_attachment)) &&
        (desc.depth != 1 || desc.mip_levels != 1))
    {
        fail("render attachments must be 2D single-mip textures");
    }
    if ((has_flag(desc.usage, TextureUsage::color_attachment) ||
         has_flag(desc.usage, TextureUsage::depth_attachment)) &&
        (desc.width > impl_->physical_properties.limits.maxFramebufferWidth ||
         desc.height > impl_->physical_properties.limits.maxFramebufferHeight))
    {
        fail("render attachment dimensions exceed Vulkan framebuffer limits");
    }
    if (has_flag(desc.usage, TextureUsage::color_attachment) ||
        has_flag(desc.usage, TextureUsage::depth_attachment))
    {
        const auto& limits = impl_->physical_properties.limits;
        const auto viewport_width = static_cast<float>(desc.width);
        const auto viewport_height = static_cast<float>(desc.height);
        if (desc.width > limits.maxViewportDimensions[0] ||
            desc.height > limits.maxViewportDimensions[1] ||
            limits.viewportBoundsRange[0] > 0.0f ||
            viewport_width > limits.viewportBoundsRange[1] ||
            viewport_height > limits.viewportBoundsRange[1])
        {
            fail("render attachment dimensions cannot form the emitted Vulkan viewport");
        }
    }

    auto result = std::make_shared<Texture::Impl>();
    result->state = impl_;
    result->desc = desc;
    result->desc.name = {};

    VkImageUsageFlags usage = 0;
    if (has_flag(desc.usage, TextureUsage::sampled)) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (has_flag(desc.usage, TextureUsage::storage)) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (has_flag(desc.usage, TextureUsage::color_attachment))
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (has_flag(desc.usage, TextureUsage::depth_attachment))
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (has_flag(desc.usage, TextureUsage::transfer_source))
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (has_flag(desc.usage, TextureUsage::transfer_destination))
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (usage == 0)
        fail("texture must have at least one usage");

    const auto image_type = desc.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    const auto format = to_vk(desc.format);
    const auto format_features = optimal_format_features(impl_->physical_device, desc.format);
    const auto required_features = required_format_features(desc.usage);
    if ((format_features & required_features) != required_features)
    {
        fail("texture format does not support all requested usages (sampled textures also "
             "require linear filtering; storage textures require formatless read/write)");
    }

    VkImageFormatProperties image_properties{};
    const auto format_result = vkGetPhysicalDeviceImageFormatProperties(
        impl_->physical_device,
        format,
        image_type,
        VK_IMAGE_TILING_OPTIMAL,
        usage,
        0,
        &image_properties);
    if (format_result == VK_ERROR_FORMAT_NOT_SUPPORTED)
        fail("texture format/type/usage combination is not supported by the device");
    check(format_result, "vkGetPhysicalDeviceImageFormatProperties");
    if (desc.width > image_properties.maxExtent.width ||
        desc.height > image_properties.maxExtent.height ||
        desc.depth > image_properties.maxExtent.depth ||
        desc.mip_levels > image_properties.maxMipLevels ||
        (image_properties.sampleCounts & VK_SAMPLE_COUNT_1_BIT) == 0 ||
        image_resource_byte_size(desc) > image_properties.maxResourceSize)
    {
        fail("texture dimensions, mip count, or resource size exceed device format limits");
    }

    const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = image_type,
        .format = format,
        .extent = {desc.width, desc.height, desc.depth},
        .mipLevels = desc.mip_levels,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    check(vkCreateImage(impl_->device, &image_info, nullptr, &result->image),
          "vkCreateImage");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(impl_->device, result->image, &requirements);
    const auto memory_type = impl_->find_memory_type(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    const VkMemoryDedicatedAllocateInfo dedicated_info{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = nullptr,
        .image = result->image,
        .buffer = VK_NULL_HANDLE,
    };
    const VkMemoryAllocateInfo allocate_info{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated_info,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type,
    };
    result->memory = impl_->allocate_memory(allocate_info);
    check(vkBindImageMemory(impl_->device, result->image, result->memory, 0),
          "vkBindImageMemory");

    const bool depth = desc.format == Format::d32_float;
    const VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = result->image,
        .viewType = desc.depth > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D,
        .format = to_vk(desc.format),
        .components = {},
        .subresourceRange = {
            .aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = desc.mip_levels,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    check(vkCreateImageView(impl_->device, &view_info, nullptr, &result->view),
          "vkCreateImageView");

    if (has_flag(desc.usage, TextureUsage::sampled))
    {
        result->sampled_index = impl_->resource_heap.allocate();
        const VkImageDescriptorInfoEXT image_descriptor{
            .sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT,
            .pNext = nullptr,
            .pView = &view_info,
            .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkResourceDescriptorDataEXT data{};
        data.pImage = &image_descriptor;
        const VkResourceDescriptorInfoEXT descriptor_info{
            .sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
            .pNext = nullptr,
            .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .data = data,
        };
        const VkHostAddressRangeEXT destination{
            .address = impl_->resource_heap.host_address(result->sampled_index),
            .size = static_cast<std::size_t>(impl_->resource_heap.descriptor_size),
        };
        {
            const std::scoped_lock lock(impl_->resource_heap.mutex);
            check(impl_->fn.write_resource_descriptors(
                      impl_->device, 1, &descriptor_info, &destination),
                  "vkWriteResourceDescriptorsEXT");
            impl_->flush(impl_->resource_heap.backing);
        }
    }

    if (has_flag(desc.usage, TextureUsage::storage))
    {
        result->storage_index = impl_->resource_heap.allocate();
        const VkImageDescriptorInfoEXT image_descriptor{
            .sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT,
            .pNext = nullptr,
            .pView = &view_info,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        };
        VkResourceDescriptorDataEXT data{};
        data.pImage = &image_descriptor;
        const VkResourceDescriptorInfoEXT descriptor_info{
            .sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
            .pNext = nullptr,
            .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .data = data,
        };
        const VkHostAddressRangeEXT destination{
            .address = impl_->resource_heap.host_address(result->storage_index),
            .size = static_cast<std::size_t>(impl_->resource_heap.descriptor_size),
        };
        {
            const std::scoped_lock lock(impl_->resource_heap.mutex);
            check(impl_->fn.write_resource_descriptors(
                      impl_->device, 1, &descriptor_info, &destination),
                  "vkWriteResourceDescriptorsEXT(storage image)");
            impl_->flush(impl_->resource_heap.backing);
        }
    }

    return Texture{std::move(result)};
}

Sampler Device::create_sampler(const SamplerDesc& desc) const
{
    if (!impl_)
        fail("create_sampler called on an empty device");

    auto result = std::make_unique<Sampler::Impl>();
    result->state = impl_;
    result->index = impl_->sampler_heap.allocate();
    const VkSamplerCreateInfo sampler_info{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = to_vk(desc.mag_filter),
        .minFilter = to_vk(desc.min_filter),
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = to_vk(desc.address_u),
        .addressModeV = to_vk(desc.address_v),
        .addressModeW = to_vk(desc.address_w),
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .minLod = 0.0f,
        .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };
    const VkHostAddressRangeEXT destination{
        .address = impl_->sampler_heap.host_address(result->index),
        .size = static_cast<std::size_t>(impl_->sampler_heap.descriptor_size),
    };
    {
        const std::scoped_lock lock(impl_->sampler_heap.mutex);
        check(impl_->fn.write_sampler_descriptors(
                  impl_->device, 1, &sampler_info, &destination),
              "vkWriteSamplerDescriptorsEXT");
        impl_->flush(impl_->sampler_heap.backing);
    }
    return Sampler{std::move(result)};
}

Pipeline Device::create_graphics_pipeline(const GraphicsPipelineDesc& desc) const
{
    if (!impl_)
        fail("create_graphics_pipeline called on an empty device");
    if (desc.color_format == Format::d32_float)
        fail("graphics pipeline color format must not be a depth format");
    if (desc.depth_enabled && desc.depth_format != Format::d32_float)
        fail("graphics pipeline depth format must be d32_float");
    if (desc.depth_write && !desc.depth_enabled)
        fail("depth_write requires depth_enabled");
    if ((optimal_format_features(impl_->physical_device, desc.color_format) &
         VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT) == 0)
    {
        fail("graphics pipeline color format lacks color-attachment support");
    }
    if (desc.depth_enabled &&
        (optimal_format_features(impl_->physical_device, desc.depth_format) &
         VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT) == 0)
    {
        fail("graphics pipeline depth format lacks depth-attachment support");
    }

    VkShaderModule vertex_module = VK_NULL_HANDLE;
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    try
    {
        vertex_module = create_shader_module(impl_->device, desc.vertex_spirv);
        fragment_module = create_shader_module(impl_->device, desc.fragment_spirv);

        const auto mappings = make_standard_mappings(*impl_);
        std::array mapping_infos{
            VkShaderDescriptorSetAndBindingMappingInfoEXT{
                .sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
                .pNext = nullptr,
                .mappingCount = static_cast<std::uint32_t>(mappings.size()),
                .pMappings = mappings.data(),
            },
            VkShaderDescriptorSetAndBindingMappingInfoEXT{
                .sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
                .pNext = nullptr,
                .mappingCount = static_cast<std::uint32_t>(mappings.size()),
                .pMappings = mappings.data(),
            },
        };
        const std::array stages{
            VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = &mapping_infos[0],
                .flags = 0,
                .stage = VK_SHADER_STAGE_VERTEX_BIT,
                .module = vertex_module,
                .pName = "vertexMain",
                .pSpecializationInfo = nullptr,
            },
            VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = &mapping_infos[1],
                .flags = 0,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = fragment_module,
                .pName = "fragmentMain",
                .pSpecializationInfo = nullptr,
            },
        };
        const VkPipelineVertexInputStateCreateInfo vertex_input{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .vertexBindingDescriptionCount = 0,
            .pVertexBindingDescriptions = nullptr,
            .vertexAttributeDescriptionCount = 0,
            .pVertexAttributeDescriptions = nullptr,
        };
        const VkPipelineInputAssemblyStateCreateInfo input_assembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .topology = to_vk(desc.topology),
            .primitiveRestartEnable = VK_FALSE,
        };
        const VkPipelineViewportStateCreateInfo viewport_state{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .viewportCount = 1,
            .pViewports = nullptr,
            .scissorCount = 1,
            .pScissors = nullptr,
        };
        const VkPipelineRasterizationStateCreateInfo rasterization{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .depthClampEnable = VK_FALSE,
            .rasterizerDiscardEnable = VK_FALSE,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = to_vk(desc.cull),
            .frontFace = desc.cull == CullMode::counter_clockwise
                             ? VK_FRONT_FACE_CLOCKWISE
                             : VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .depthBiasEnable = VK_FALSE,
            .depthBiasConstantFactor = 0.0f,
            .depthBiasClamp = 0.0f,
            .depthBiasSlopeFactor = 0.0f,
            .lineWidth = 1.0f,
        };
        const VkPipelineMultisampleStateCreateInfo multisample{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            .sampleShadingEnable = VK_FALSE,
            .minSampleShading = 0.0f,
            .pSampleMask = nullptr,
            .alphaToCoverageEnable = VK_FALSE,
            .alphaToOneEnable = VK_FALSE,
        };
        const VkPipelineDepthStencilStateCreateInfo depth_stencil{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .depthTestEnable = desc.depth_enabled ? VK_TRUE : VK_FALSE,
            .depthWriteEnable = desc.depth_write ? VK_TRUE : VK_FALSE,
            .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
            .depthBoundsTestEnable = VK_FALSE,
            .stencilTestEnable = VK_FALSE,
            .front = {},
            .back = {},
            .minDepthBounds = 0.0f,
            .maxDepthBounds = 1.0f,
        };
        const VkPipelineColorBlendAttachmentState color_attachment{
            .blendEnable = VK_FALSE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        };
        const VkPipelineColorBlendStateCreateInfo color_blend{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .logicOpEnable = VK_FALSE,
            .logicOp = VK_LOGIC_OP_COPY,
            .attachmentCount = 1,
            .pAttachments = &color_attachment,
            .blendConstants = {},
        };
        constexpr std::array dynamic_states{
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
        };
        const VkPipelineDynamicStateCreateInfo dynamic_state{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size()),
            .pDynamicStates = dynamic_states.data(),
        };
        const auto color_format = to_vk(desc.color_format);
        const auto depth_format = desc.depth_enabled ? to_vk(desc.depth_format) : VK_FORMAT_UNDEFINED;
        const VkPipelineRenderingCreateInfo rendering_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .pNext = nullptr,
            .viewMask = 0,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &color_format,
            .depthAttachmentFormat = depth_format,
            .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
        };
        const VkPipelineCreateFlags2CreateInfo flags_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
            .pNext = &rendering_info,
            .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
        };
        const VkGraphicsPipelineCreateInfo pipeline_info{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &flags_info,
            .flags = 0,
            .stageCount = static_cast<std::uint32_t>(stages.size()),
            .pStages = stages.data(),
            .pVertexInputState = &vertex_input,
            .pInputAssemblyState = &input_assembly,
            .pTessellationState = nullptr,
            .pViewportState = &viewport_state,
            .pRasterizationState = &rasterization,
            .pMultisampleState = &multisample,
            .pDepthStencilState = &depth_stencil,
            .pColorBlendState = &color_blend,
            .pDynamicState = &dynamic_state,
            .layout = VK_NULL_HANDLE,
            .renderPass = VK_NULL_HANDLE,
            .subpass = 0,
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = -1,
        };
        auto result = std::make_shared<Pipeline::Impl>();
        result->state = impl_;
        result->bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
        result->color_format = desc.color_format;
        result->depth_format = desc.depth_format;
        result->depth_enabled = desc.depth_enabled;
        check(vkCreateGraphicsPipelines(
                  impl_->device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &result->pipeline),
              "vkCreateGraphicsPipelines");

        vkDestroyShaderModule(impl_->device, fragment_module, nullptr);
        vkDestroyShaderModule(impl_->device, vertex_module, nullptr);
        return Pipeline{std::move(result)};
    }
    catch (...)
    {
        if (fragment_module)
            vkDestroyShaderModule(impl_->device, fragment_module, nullptr);
        if (vertex_module)
            vkDestroyShaderModule(impl_->device, vertex_module, nullptr);
        throw;
    }
}

Pipeline Device::create_compute_pipeline(const ComputePipelineDesc& desc) const
{
    if (!impl_)
        fail("create_compute_pipeline called on an empty device");

    VkShaderModule module = VK_NULL_HANDLE;
    try
    {
        module = create_shader_module(impl_->device, desc.compute_spirv);
        const auto mappings = make_standard_mappings(*impl_);
        const VkShaderDescriptorSetAndBindingMappingInfoEXT mapping_info{
            .sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
            .pNext = nullptr,
            .mappingCount = static_cast<std::uint32_t>(mappings.size()),
            .pMappings = mappings.data(),
        };
        const VkPipelineShaderStageCreateInfo stage{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = &mapping_info,
            .flags = 0,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = module,
            .pName = "computeMain",
            .pSpecializationInfo = nullptr,
        };
        const VkPipelineCreateFlags2CreateInfo flags_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
        };
        const VkComputePipelineCreateInfo pipeline_info{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .pNext = &flags_info,
            .flags = 0,
            .stage = stage,
            .layout = VK_NULL_HANDLE,
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = -1,
        };
        auto result = std::make_shared<Pipeline::Impl>();
        result->state = impl_;
        result->bind_point = VK_PIPELINE_BIND_POINT_COMPUTE;
        check(vkCreateComputePipelines(
                  impl_->device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &result->pipeline),
              "vkCreateComputePipelines");
        vkDestroyShaderModule(impl_->device, module, nullptr);
        return Pipeline{std::move(result)};
    }
    catch (...)
    {
        if (module)
            vkDestroyShaderModule(impl_->device, module, nullptr);
        throw;
    }
}

CommandList Device::begin_commands() const
{
    if (!impl_)
        fail("begin_commands called on an empty device");

    auto result = std::make_unique<CommandList::Impl>();
    result->state = impl_;
    const VkCommandPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = impl_->queue_family,
    };
    check(vkCreateCommandPool(impl_->device, &pool_info, nullptr, &result->command_pool),
          "vkCreateCommandPool");
    const VkCommandBufferAllocateInfo allocate_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = result->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    check(vkAllocateCommandBuffers(
              impl_->device, &allocate_info, &result->command_buffer),
          "vkAllocateCommandBuffers");
    const VkCommandBufferBeginInfo begin_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    check(vkBeginCommandBuffer(result->command_buffer, &begin_info), "vkBeginCommandBuffer");

    const auto sampler_bind = impl_->sampler_heap.bind_info();
    const auto resource_bind = impl_->resource_heap.bind_info();
    impl_->fn.cmd_bind_sampler_heap(result->command_buffer, &sampler_bind);
    impl_->fn.cmd_bind_resource_heap(result->command_buffer, &resource_bind);
    return CommandList{std::move(result)};
}

void Device::submit_and_wait(CommandList&& commands) const
{
    if (!impl_ || !commands.impl_)
        fail("submit_and_wait received an empty device or command list");
    if (commands.impl_->state.get() != impl_.get())
        fail("command list belongs to a different device");

    auto owned = std::move(commands.impl_);
    if (owned->recording)
    {
        if (owned->rendering)
            fail("cannot submit while a rendering scope is active");
        check(vkEndCommandBuffer(owned->command_buffer), "vkEndCommandBuffer");
        owned->recording = false;
    }

    const VkFenceCreateInfo fence_info{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
    };
    VkFence fence = VK_NULL_HANDLE;
    check(vkCreateFence(impl_->device, &fence_info, nullptr, &fence), "vkCreateFence");
    try
    {
        const VkSubmitInfo submit_info{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = nullptr,
            .waitSemaphoreCount = 0,
            .pWaitSemaphores = nullptr,
            .pWaitDstStageMask = nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers = &owned->command_buffer,
            .signalSemaphoreCount = 0,
            .pSignalSemaphores = nullptr,
        };
        {
            const std::scoped_lock lock(impl_->queue_mutex);
            check(vkQueueSubmit(impl_->queue, 1, &submit_info, fence), "vkQueueSubmit");
        }
        check(vkWaitForFences(
                  impl_->device, 1, &fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()),
              "vkWaitForFences");
        vkDestroyFence(impl_->device, fence, nullptr);
    }
    catch (...)
    {
        vkDestroyFence(impl_->device, fence, nullptr);
        throw;
    }
}

void Device::wait_idle() const
{
    if (!impl_)
        return;
    const std::scoped_lock lock(impl_->queue_mutex);
    check(vkDeviceWaitIdle(impl_->device), "vkDeviceWaitIdle");
}

const DeviceCaps& Device::caps() const
{
    if (!impl_)
        fail("caps called on an empty device");
    return impl_->caps;
}

void CommandList::bind_pipeline(const Pipeline& pipeline)
{
    if (!impl_ || !impl_->recording || !pipeline.impl_)
        fail("bind_pipeline received an empty command list or pipeline");
    if (pipeline.impl_->state.get() != impl_->state.get())
        fail("pipeline belongs to a different device");
    vkCmdBindPipeline(
        impl_->command_buffer, pipeline.impl_->bind_point, pipeline.impl_->pipeline);
    impl_->retained_resources.emplace_back(pipeline.impl_);
    if (pipeline.impl_->bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS)
        impl_->bound_graphics = pipeline.impl_;
    else
        impl_->bound_compute = pipeline.impl_;
}

void CommandList::validate_and_retain(const BufferSlice& slice)
{
    if (!impl_ || !impl_->recording || !slice.allocation_)
        fail("address commands require a live Buffer::slice() from a recording device");
    if (slice.allocation_->state.get() != impl_->state.get())
        fail("buffer slice belongs to a different device");
    const auto& backing = slice.allocation_->backing;
    if (slice.address_ < backing.address)
        fail("buffer slice address precedes its allocation");
    const auto offset = slice.address_ - backing.address;
    if (offset > backing.size || slice.size_ > backing.size - offset)
        fail("buffer slice exceeds its allocation");
    impl_->retained_resources.emplace_back(slice.allocation_);
}

void CommandList::retain(Texture& texture)
{
    if (!texture.impl_ || texture.impl_->state.get() != impl_->state.get())
        fail("texture is empty or belongs to a different device");
    impl_->retained_resources.emplace_back(texture.impl_);
}

void CommandList::require_graphics_pipeline() const
{
    if (!impl_->bound_graphics)
        fail("draw requires a bound graphics pipeline");
    if (impl_->bound_graphics->color_format != impl_->rendering_color_format ||
        impl_->bound_graphics->depth_enabled != impl_->rendering_has_depth ||
        (impl_->rendering_has_depth &&
         impl_->bound_graphics->depth_format != impl_->rendering_depth_format))
    {
        fail("graphics pipeline formats do not match the active rendering attachments");
    }
}

void CommandList::require_compute_pipeline() const
{
    if (!impl_->bound_compute)
        fail("dispatch requires a bound compute pipeline");
}

void CommandList::push_data(std::span<const std::byte> bytes, std::uint32_t offset)
{
    if (!impl_ || !impl_->recording)
        fail("push_data requires a recording command list");
    if (bytes.empty())
        return;
    if ((offset & 3u) != 0 || (bytes.size() & 3u) != 0)
        fail("vkCmdPushDataEXT offset and size must be multiples of four bytes");
    if (bytes.size() > std::numeric_limits<std::uint32_t>::max() ||
        offset > impl_->state->caps.max_push_data_size ||
        bytes.size() > impl_->state->caps.max_push_data_size - offset)
    {
        fail("root data exceeds VkPhysicalDeviceDescriptorHeapPropertiesEXT::maxPushDataSize");
    }

    const VkPushDataInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT,
        .pNext = nullptr,
        .offset = offset,
        .data = {
            .address = bytes.data(),
            .size = bytes.size(),
        },
    };
    impl_->state->fn.cmd_push_data(impl_->command_buffer, &info);
}

void CommandList::begin_rendering(Texture& color,
                                  float4 clear_color,
                                  bool clear,
                                  Texture* depth,
                                  float clear_depth)
{
    if (!impl_ || !impl_->recording || !color.impl_)
        fail("begin_rendering received an empty object");
    if (impl_->rendering)
        fail("a rendering scope is already active");
    if (color.impl_->state.get() != impl_->state.get())
        fail("render target belongs to a different device");
    if (!has_flag(color.impl_->desc.usage, TextureUsage::color_attachment))
        fail("texture was not created for color-attachment use");
    if (depth)
    {
        if (!depth->impl_ || depth->impl_->state.get() != impl_->state.get())
            fail("depth target is empty or belongs to a different device");
        if (!has_flag(depth->impl_->desc.usage, TextureUsage::depth_attachment) ||
            depth->impl_->desc.format != Format::d32_float)
        {
            fail("depth target must be a D32 texture created for depth-attachment use");
        }
        if (depth->impl_->desc.width != color.impl_->desc.width ||
            depth->impl_->desc.height != color.impl_->desc.height)
        {
            fail("color and depth target dimensions differ");
        }
    }
    if (depth && clear && !(clear_depth >= 0.0f && clear_depth <= 1.0f))
        fail("clear depth must be in the [0, 1] range");

    retain(color);
    if (depth)
        retain(*depth);

    const VkRenderingAttachmentInfo attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = color.impl_->view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = {{clear_color.x, clear_color.y, clear_color.z, clear_color.w}}},
    };
    const VkRenderingAttachmentInfo depth_attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = depth ? depth->impl_->view : VK_NULL_HANDLE,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.depthStencil = {clear_depth, 0}},
    };
    const VkRenderingInfo rendering_info{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderArea = {{0, 0}, {color.impl_->desc.width, color.impl_->desc.height}},
        .layerCount = 1,
        .viewMask = 0,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
        .pDepthAttachment = depth ? &depth_attachment : nullptr,
        .pStencilAttachment = nullptr,
    };
    vkCmdBeginRendering(impl_->command_buffer, &rendering_info);

    const VkViewport viewport{
        .x = 0.0f,
        .y = 0.0f,
        .width = static_cast<float>(color.impl_->desc.width),
        .height = static_cast<float>(color.impl_->desc.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    const VkRect2D scissor{{0, 0}, {color.impl_->desc.width, color.impl_->desc.height}};
    vkCmdSetViewport(impl_->command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(impl_->command_buffer, 0, 1, &scissor);
    impl_->rendering = true;
    impl_->rendering_color_format = color.impl_->desc.format;
    impl_->rendering_has_depth = depth != nullptr;
    if (depth)
        impl_->rendering_depth_format = depth->impl_->desc.format;
}

void CommandList::end_rendering()
{
    if (!impl_ || !impl_->recording || !impl_->rendering)
        fail("end_rendering requires an active rendering scope");
    vkCmdEndRendering(impl_->command_buffer);
    impl_->rendering = false;
    impl_->rendering_has_depth = false;
}

void CommandList::draw(std::uint32_t vertex_count,
                       std::uint32_t instance_count,
                       std::uint32_t first_vertex,
                       std::uint32_t first_instance)
{
    if (!impl_ || !impl_->recording || !impl_->rendering)
        fail("draw requires an active rendering scope");
    require_graphics_pipeline();
    vkCmdDraw(impl_->command_buffer,
              vertex_count,
              instance_count,
              first_vertex,
              first_instance);
}

void CommandList::draw_indexed(BufferSlice indices,
                               IndexType type,
                               std::uint32_t index_count,
                               std::uint32_t instance_count,
                               std::uint32_t first_index,
                               std::int32_t vertex_offset,
                               std::uint32_t first_instance)
{
    if (!impl_ || !impl_->recording || !impl_->rendering)
        fail("draw_indexed requires an active rendering scope");
    require_graphics_pipeline();
    validate_and_retain(indices);
    const auto alignment = type == IndexType::uint16 ? 2u : 4u;
    if (indices.address_ == 0 || indices.size_ == 0 || indices.address_ % alignment != 0)
        fail("index address range is empty or misaligned");
    const auto index_size = static_cast<std::uint64_t>(alignment);
    if (first_index > indices.size_ / index_size ||
        index_count > (indices.size_ - static_cast<std::uint64_t>(first_index) * index_size) /
                          index_size)
    {
        fail("indexed draw exceeds the bound index address range");
    }

    const VkBindIndexBuffer3InfoKHR bind_info{
        .sType = VK_STRUCTURE_TYPE_BIND_INDEX_BUFFER_3_INFO_KHR,
        .pNext = nullptr,
        .addressRange = {indices.address_, indices.size_},
        .addressFlags = address_flags,
        .indexType = type == IndexType::uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32,
    };
    impl_->state->fn.cmd_bind_index_buffer(impl_->command_buffer, &bind_info);
    vkCmdDrawIndexed(impl_->command_buffer,
                     index_count,
                     instance_count,
                     first_index,
                     vertex_offset,
                     first_instance);
}

void CommandList::draw_indirect(BufferSlice arguments,
                                std::uint32_t draw_count,
                                std::uint32_t stride)
{
    if (!impl_ || !impl_->recording || !impl_->rendering)
        fail("draw_indirect requires an active rendering scope");
    require_graphics_pipeline();
    validate_and_retain(arguments);
    if (stride == 0)
        stride = sizeof(VkDrawIndirectCommand);
    const auto required_size = draw_count == 0
                                   ? 0ull
                                   : static_cast<std::uint64_t>(draw_count - 1) * stride +
                                         sizeof(VkDrawIndirectCommand);
    if (draw_count == 0 || arguments.address_ == 0 || (arguments.address_ & 3u) != 0 ||
        stride < sizeof(VkDrawIndirectCommand) || (stride & 3u) != 0 ||
        stride > arguments.size_ || arguments.size_ < required_size ||
        draw_count > impl_->state->physical_properties.limits.maxDrawIndirectCount)
    {
        fail("indirect draw range, count, or stride is invalid");
    }
    const VkDrawIndirect2InfoKHR info{
        .sType = VK_STRUCTURE_TYPE_DRAW_INDIRECT_2_INFO_KHR,
        .pNext = nullptr,
        .addressRange = {arguments.address_, arguments.size_, stride},
        .addressFlags = address_flags,
        .drawCount = draw_count,
    };
    impl_->state->fn.cmd_draw_indirect(impl_->command_buffer, &info);
}

void CommandList::draw_indexed_indirect(BufferSlice indices,
                                        IndexType type,
                                        BufferSlice arguments,
                                        std::uint32_t draw_count,
                                        std::uint32_t stride)
{
    if (!impl_ || !impl_->recording || !impl_->rendering)
        fail("draw_indexed_indirect requires an active rendering scope");
    require_graphics_pipeline();
    validate_and_retain(indices);
    validate_and_retain(arguments);
    const auto index_alignment = type == IndexType::uint16 ? 2u : 4u;
    if (indices.address_ == 0 || indices.size_ == 0 ||
        indices.address_ % index_alignment != 0)
    {
        fail("index address range is empty or misaligned");
    }
    if (stride == 0)
        stride = sizeof(VkDrawIndexedIndirectCommand);
    const auto required_size = draw_count == 0
                                   ? 0ull
                                   : static_cast<std::uint64_t>(draw_count - 1) * stride +
                                         sizeof(VkDrawIndexedIndirectCommand);
    if (draw_count == 0 || arguments.address_ == 0 || (arguments.address_ & 3u) != 0 ||
        stride < sizeof(VkDrawIndexedIndirectCommand) || (stride & 3u) != 0 ||
        stride > arguments.size_ || arguments.size_ < required_size ||
        draw_count > impl_->state->physical_properties.limits.maxDrawIndirectCount)
    {
        fail("indexed indirect draw range, count, or stride is invalid");
    }

    const VkBindIndexBuffer3InfoKHR bind_info{
        .sType = VK_STRUCTURE_TYPE_BIND_INDEX_BUFFER_3_INFO_KHR,
        .pNext = nullptr,
        .addressRange = {indices.address_, indices.size_},
        .addressFlags = address_flags,
        .indexType = type == IndexType::uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32,
    };
    impl_->state->fn.cmd_bind_index_buffer(impl_->command_buffer, &bind_info);
    const VkDrawIndirect2InfoKHR info{
        .sType = VK_STRUCTURE_TYPE_DRAW_INDIRECT_2_INFO_KHR,
        .pNext = nullptr,
        .addressRange = {arguments.address_, arguments.size_, stride},
        .addressFlags = address_flags,
        .drawCount = draw_count,
    };
    impl_->state->fn.cmd_draw_indexed_indirect(impl_->command_buffer, &info);
}

void CommandList::dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z)
{
    if (!impl_ || !impl_->recording || impl_->rendering)
        fail("dispatch requires a recording command list outside rendering");
    require_compute_pipeline();
    const auto& limits = impl_->state->physical_properties.limits.maxComputeWorkGroupCount;
    if (x > limits[0] || y > limits[1] || z > limits[2])
        fail("dispatch group count exceeds VkPhysicalDeviceLimits::maxComputeWorkGroupCount");
    vkCmdDispatch(impl_->command_buffer, x, y, z);
}

void CommandList::dispatch_indirect(BufferSlice arguments)
{
    if (!impl_ || !impl_->recording || impl_->rendering)
        fail("dispatch_indirect requires a recording command list outside rendering");
    require_compute_pipeline();
    validate_and_retain(arguments);
    if (arguments.address_ == 0 || arguments.size_ < sizeof(VkDispatchIndirectCommand) ||
        (arguments.address_ & 3u) != 0)
    {
        fail("indirect dispatch range is too small or misaligned");
    }
    const VkDispatchIndirect2InfoKHR info{
        .sType = VK_STRUCTURE_TYPE_DISPATCH_INDIRECT_2_INFO_KHR,
        .pNext = nullptr,
        .addressRange = {arguments.address_, arguments.size_},
        .addressFlags = address_flags,
    };
    impl_->state->fn.cmd_dispatch_indirect(impl_->command_buffer, &info);
}

void CommandList::copy_buffer(BufferSlice source,
                              BufferSlice destination,
                              std::uint64_t byte_count)
{
    if (!impl_ || !impl_->recording || impl_->rendering)
        fail("copy_buffer requires a recording command list outside rendering");
    validate_and_retain(source);
    validate_and_retain(destination);
    if (byte_count == 0)
        byte_count = std::min(source.size_, destination.size_);
    if (source.address_ == 0 || destination.address_ == 0 || byte_count == 0 ||
        byte_count > source.size_ || byte_count > destination.size_)
    {
        fail("invalid device-address range passed to copy_buffer");
    }
    if (source.allocation_.get() == destination.allocation_.get() &&
        source.address_ < destination.address_ + byte_count &&
        destination.address_ < source.address_ + byte_count)
    {
        fail("copy_buffer source and destination ranges overlap");
    }

    const VkDeviceMemoryCopyKHR region{
        .sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_COPY_KHR,
        .pNext = nullptr,
        .srcRange = {source.address_, byte_count},
        .srcFlags = address_flags,
        .dstRange = {destination.address_, byte_count},
        .dstFlags = address_flags,
    };
    const VkCopyDeviceMemoryInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_INFO_KHR,
        .pNext = nullptr,
        .regionCount = 1,
        .pRegions = &region,
    };
    impl_->state->fn.cmd_copy_memory(impl_->command_buffer, &info);
}

void CommandList::copy_buffer_to_texture(BufferSlice source, Texture& destination)
{
    if (!impl_ || !impl_->recording || impl_->rendering || !destination.impl_)
        fail("copy_buffer_to_texture received an empty object or active rendering scope");
    validate_and_retain(source);
    retain(destination);
    if (destination.impl_->state.get() != impl_->state.get())
        fail("texture belongs to a different device");
    if (!has_flag(destination.impl_->desc.usage, TextureUsage::transfer_destination))
        fail("texture was not created as a transfer destination");
    const auto texel_size = bytes_per_texel(destination.impl_->desc.format);
    if (source.address_ == 0 || source.address_ % texel_size != 0 ||
        source.size_ < base_level_byte_size(destination.impl_->desc))
        fail("source range is empty, misaligned, or too small for the texture base level");

    const bool depth = destination.impl_->desc.format == Format::d32_float;
    const VkDeviceMemoryImageCopyKHR region{
        .sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_IMAGE_COPY_KHR,
        .pNext = nullptr,
        .addressRange = {source.address_, source.size_},
        .addressFlags = address_flags,
        .addressRowLength = 0,
        .addressImageHeight = 0,
        .imageSubresource = {
            .aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .imageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .imageOffset = {0, 0, 0},
        .imageExtent = {destination.impl_->desc.width,
                        destination.impl_->desc.height,
                        destination.impl_->desc.depth},
    };
    const VkCopyDeviceMemoryImageInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_IMAGE_INFO_KHR,
        .pNext = nullptr,
        .image = destination.impl_->image,
        .regionCount = 1,
        .pRegions = &region,
    };
    impl_->state->fn.cmd_copy_memory_to_image(impl_->command_buffer, &info);
}

void CommandList::copy_texture_to_buffer(Texture& source, BufferSlice destination)
{
    if (!impl_ || !impl_->recording || impl_->rendering || !source.impl_)
        fail("copy_texture_to_buffer received an empty object or active rendering scope");
    validate_and_retain(destination);
    retain(source);
    if (source.impl_->state.get() != impl_->state.get())
        fail("texture belongs to a different device");
    if (!has_flag(source.impl_->desc.usage, TextureUsage::transfer_source))
        fail("texture was not created as a transfer source");
    const auto texel_size = bytes_per_texel(source.impl_->desc.format);
    if (destination.address_ == 0 || destination.address_ % texel_size != 0 ||
        destination.size_ < base_level_byte_size(source.impl_->desc))
    {
        fail("destination range is empty, misaligned, or too small for the texture base level");
    }

    const bool depth = source.impl_->desc.format == Format::d32_float;
    const VkDeviceMemoryImageCopyKHR region{
        .sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_IMAGE_COPY_KHR,
        .pNext = nullptr,
        .addressRange = {destination.address_, destination.size_},
        .addressFlags = address_flags,
        .addressRowLength = 0,
        .addressImageHeight = 0,
        .imageSubresource = {
            .aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .imageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .imageOffset = {0, 0, 0},
        .imageExtent = {source.impl_->desc.width,
                        source.impl_->desc.height,
                        source.impl_->desc.depth},
    };
    const VkCopyDeviceMemoryImageInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_IMAGE_INFO_KHR,
        .pNext = nullptr,
        .image = source.impl_->image,
        .regionCount = 1,
        .pRegions = &region,
    };
    impl_->state->fn.cmd_copy_image_to_memory(impl_->command_buffer, &info);
}

void CommandList::transition(Texture& texture, ImageState before, ImageState after)
{
    if (!impl_ || !impl_->recording || impl_->rendering || !texture.impl_)
        fail("transition requires a recording command list outside rendering");
    if (texture.impl_->state.get() != impl_->state.get())
        fail("texture belongs to a different device");
    if (after == ImageState::undefined)
        fail("undefined is only valid as the source state of an image transition");
    validate_image_state(texture.impl_->desc.usage, before);
    validate_image_state(texture.impl_->desc.usage, after);
    retain(texture);

    const auto source = state_info(before);
    const auto destination = state_info(after);
    const bool depth = texture.impl_->desc.format == Format::d32_float;
    const VkImageMemoryBarrier2 image_barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = source.stage,
        .srcAccessMask = source.access,
        .dstStageMask = destination.stage,
        .dstAccessMask = destination.access,
        .oldLayout = source.layout,
        .newLayout = destination.layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = texture.impl_->image,
        .subresourceRange = {
            .aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = texture.impl_->desc.mip_levels,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .dependencyFlags = 0,
        .memoryBarrierCount = 0,
        .pMemoryBarriers = nullptr,
        .bufferMemoryBarrierCount = 0,
        .pBufferMemoryBarriers = nullptr,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &image_barrier,
    };
    vkCmdPipelineBarrier2(impl_->command_buffer, &dependency);
}

void CommandList::barrier(Stage before,
                          Access before_access,
                          Stage after,
                          Access after_access)
{
    if (!impl_ || !impl_->recording || impl_->rendering)
        fail("barrier requires a recording command list outside rendering");
    validate_stage_access(before, before_access);
    validate_stage_access(after, after_access);
    const VkMemoryBarrier2 memory_barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = to_vk(before),
        .srcAccessMask = to_vk(before_access),
        .dstStageMask = to_vk(after),
        .dstAccessMask = to_vk(after_access),
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .dependencyFlags = 0,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &memory_barrier,
        .bufferMemoryBarrierCount = 0,
        .pBufferMemoryBarriers = nullptr,
        .imageMemoryBarrierCount = 0,
        .pImageMemoryBarriers = nullptr,
    };
    vkCmdPipelineBarrier2(impl_->command_buffer, &dependency);
}

void CommandList::finish()
{
    if (!impl_ || !impl_->recording)
        return;
    if (impl_->rendering)
        fail("finish called while a rendering scope is active");
    check(vkEndCommandBuffer(impl_->command_buffer), "vkEndCommandBuffer");
    impl_->recording = false;
}

Buffer::Buffer() noexcept = default;
Buffer::~Buffer() = default;
Buffer::Buffer(Buffer&&) noexcept = default;
Buffer& Buffer::operator=(Buffer&&) noexcept = default;
Buffer::Buffer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

DeviceAddress Buffer::address() const noexcept
{
    return impl_ ? impl_->allocation->backing.address : 0;
}

std::uint64_t Buffer::size() const noexcept
{
    return impl_ ? impl_->allocation->backing.size : 0;
}

void* Buffer::mapped_data() noexcept
{
    return impl_ ? impl_->allocation->backing.mapped : nullptr;
}

const void* Buffer::mapped_data() const noexcept
{
    return impl_ ? impl_->allocation->backing.mapped : nullptr;
}

BufferSlice Buffer::slice(std::uint64_t offset, std::uint64_t byte_count) const
{
    if (!impl_)
        fail("slice called on an empty buffer");
    return BufferSlice{impl_->allocation->backing.address,
                       impl_->allocation->backing.size,
                       impl_->allocation}
        .subspan(offset, byte_count);
}

void Buffer::flush(std::uint64_t offset, std::uint64_t byte_count) const
{
    if (!impl_ || !impl_->allocation->backing.mapped)
        fail("flush requires a host-visible buffer");
    (void)slice(offset, byte_count);
    impl_->allocation->state->flush(impl_->allocation->backing);
}

void Buffer::invalidate(std::uint64_t offset, std::uint64_t byte_count) const
{
    if (!impl_ || !impl_->allocation->backing.mapped)
        fail("invalidate requires a host-visible buffer");
    (void)slice(offset, byte_count);
    impl_->allocation->state->invalidate(impl_->allocation->backing);
}

Buffer::operator bool() const noexcept { return impl_ != nullptr; }

Texture::Texture() noexcept = default;
Texture::~Texture() = default;
Texture::Texture(Texture&&) noexcept = default;
Texture& Texture::operator=(Texture&&) noexcept = default;
Texture::Texture(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

std::uint32_t Texture::sampled_index() const
{
    if (!impl_ || impl_->sampled_index == invalid_heap_index)
        fail("texture has no sampled descriptor");
    return impl_->sampled_index;
}

std::uint32_t Texture::storage_index() const
{
    if (!impl_ || impl_->storage_index == invalid_heap_index)
        fail("texture has no storage descriptor");
    return impl_->storage_index;
}

std::uint32_t Texture::width() const noexcept { return impl_ ? impl_->desc.width : 0; }
std::uint32_t Texture::height() const noexcept { return impl_ ? impl_->desc.height : 0; }
Texture::operator bool() const noexcept { return impl_ != nullptr; }

Sampler::Sampler() noexcept = default;
Sampler::~Sampler() = default;
Sampler::Sampler(Sampler&&) noexcept = default;
Sampler& Sampler::operator=(Sampler&&) noexcept = default;
Sampler::Sampler(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

std::uint32_t Sampler::index() const
{
    if (!impl_)
        fail("index called on an empty sampler");
    return impl_->index;
}

Sampler::operator bool() const noexcept { return impl_ != nullptr; }

Pipeline::Pipeline() noexcept = default;
Pipeline::~Pipeline() = default;
Pipeline::Pipeline(Pipeline&&) noexcept = default;
Pipeline& Pipeline::operator=(Pipeline&&) noexcept = default;
Pipeline::Pipeline(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Pipeline::operator bool() const noexcept { return impl_ != nullptr; }

CommandList::CommandList() noexcept = default;
CommandList::~CommandList() = default;
CommandList::CommandList(CommandList&&) noexcept = default;
CommandList& CommandList::operator=(CommandList&&) noexcept = default;
CommandList::CommandList(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
CommandList::operator bool() const noexcept { return impl_ != nullptr; }

Device::Device() noexcept = default;
Device::~Device() = default;
Device::Device(Device&&) noexcept = default;
Device& Device::operator=(Device&&) noexcept = default;
Device::Device(std::shared_ptr<detail::DeviceState> impl) noexcept : impl_(std::move(impl)) {}
Device::operator bool() const noexcept { return impl_ != nullptr; }

} // namespace clean_gfx
