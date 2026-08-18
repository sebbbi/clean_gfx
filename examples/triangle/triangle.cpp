#include "example_support.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>

int main(int argc, char** argv)
{
    constexpr const char* application = "clean_gfx_triangle";
    constexpr std::uint32_t width = 512;
    constexpr std::uint32_t height = 512;

    const ShaderCode vertex_spirv =
        read_spirv(application, CLEAN_GFX_VERTEX_SPV_PATH);
    const ShaderCode fragment_spirv =
        read_spirv(application, CLEAN_GFX_FRAGMENT_SPV_PATH);
    if (vertex_spirv.size == 0 || fragment_spirv.size == 0)
        return 1;

    const gfx::DeviceInit device_init = gfx::create_device({
        .application_name = "clean_gfx triangle",
    });
    if (device_init.error != gfx::Error::none)
    {
        std::fprintf(stderr,
                     "%s: create device: %s\n",
                     application,
                     gfx_error_name(device_init.error));
        return 1;
    }
    gfx::Device* device = device_init.device;
    const gfx::DeviceCaps& caps = gfx::get_device_caps(device);
    std::printf("Using %s\n", caps.device_name);

    gfx::Texture* target = gfx::create_texture(
        device,
        {
            .width = width,
            .height = height,
            .format = gfx::Format::bgra8_srgb,
            .usage = gfx::TextureUsage::color_attachment |
                     gfx::TextureUsage::transfer_source,
        });

    gfx::GpuAllocation<std::byte> readback_memory = gfx::gpu_malloc<std::byte>(
        device,
        static_cast<std::uint64_t>(width) * height * 4,
        gfx::MemoryType::readback);

    gfx::Pipeline* pipeline = gfx::create_graphics_pipeline(
        device,
        {
            .vertex_spirv = gfx::Span<const std::uint32_t>{
                vertex_spirv.words.data(), vertex_spirv.size},
            .fragment_spirv = gfx::Span<const std::uint32_t>{
                fragment_spirv.words.data(), fragment_spirv.size},
            .color_format = gfx::Format::bgra8_srgb,
            .cull = gfx::CullMode::counter_clockwise,
        });

    gfx::CommandList* commands = gfx::begin_commands(device);
    gfx::begin_rendering(
        commands,
        target,
        {.x = 0.01f, .y = 0.01f, .z = 0.033f, .w = 1.0f});
    gfx::bind_pipeline(commands, pipeline);
    gfx::draw(commands, nullptr, 3);
    gfx::end_rendering(commands);
    gfx::barrier(commands,
                 gfx::Stage::color_output,
                 gfx::Access::color_write,
                 gfx::Stage::transfer,
                 gfx::Access::transfer_read);
    gfx::copy_texture_to_memory(commands, target, gfx::gpu_range(readback_memory));
    gfx::barrier(commands,
                 gfx::Stage::transfer,
                 gfx::Access::transfer_write,
                 gfx::Stage::host,
                 gfx::Access::host_read);
    gfx::submit_and_wait(device, commands);

    const std::byte* pixels = readback_memory.cpu;
    bool succeeded = true;
    bool displayed = false;
    if (argc == 1 && example_window_supported)
    {
        ExampleWindow window =
            open_example_window("clean_gfx triangle", width, height);
        if (!window.running)
        {
            std::fprintf(stderr, "%s: could not open window\n", application);
            succeeded = false;
        }
        else
        {
            displayed = true;
            while (pump_example_window(window))
            {
                if (!present_bgra8(window, pixels, width, height))
                {
                    std::fprintf(stderr,
                                 "%s: could not present image\n",
                                 application);
                    succeeded = false;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        }
        close_example_window(window);
    }

    if (succeeded && !displayed)
    {
        const char* output_path = argc > 1 ? argv[1] : "clean_gfx_triangle.ppm";
        succeeded = write_bgra8_ppm(application, output_path, pixels, width, height);
        if (succeeded)
            std::printf("Wrote %s\n", output_path);
    }

    gfx::destroy_pipeline(pipeline);
    gfx::gpu_free(device, readback_memory);
    gfx::destroy_texture(target);
    gfx::destroy_device(device);
    return succeeded ? 0 : 1;
}
