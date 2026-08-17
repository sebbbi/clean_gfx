#include "example_support.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

int main(int argc, char** argv)
{
    constexpr const char* application = "clean_gfx_triangle";
    constexpr std::uint32_t width = 512;
    constexpr std::uint32_t height = 512;

    gfx::Device device;
    if (!gfx_succeeded(
            gfx::Device::create(
                device,
                {
                    .application_name = "clean_gfx triangle",
                }),
            application,
            "create device"))
        return 1;
    std::cout << "Using " << device.caps().device_name << '\n';

    gfx::Texture target;
    if (!gfx_succeeded(
            device.create_texture(
                target,
                {
                    .width = width,
                    .height = height,
                    .depth = 1,
                    .mip_levels = 1,
                    .format = gfx::Format::bgra8_srgb,
                    .usage = gfx::TextureUsage::color_attachment |
                             gfx::TextureUsage::transfer_source,
                }),
            application,
            "create target texture"))
        return 1;

    auto readback_memory = device.gpu_malloc<std::byte>(
        static_cast<std::uint64_t>(width) * height * 4,
        gfx::MemoryType::readback);

    std::vector<std::uint32_t> vertex_spirv;
    std::vector<std::uint32_t> fragment_spirv;
    if (!read_spirv(application, CLEAN_GFX_VERTEX_SPV_PATH, vertex_spirv) ||
        !read_spirv(application, CLEAN_GFX_FRAGMENT_SPV_PATH, fragment_spirv))
        return 1;

    gfx::Pipeline pipeline;
    if (!gfx_succeeded(
            device.create_graphics_pipeline(
                pipeline,
                {
                    .vertex_spirv = vertex_spirv,
                    .fragment_spirv = fragment_spirv,
                    .color_format = gfx::Format::bgra8_srgb,
                    .depth_enabled = false,
                    .topology = gfx::PrimitiveTopology::triangle_list,
                    .cull = gfx::CullMode::counter_clockwise,
                }),
            application,
            "create graphics pipeline"))
        return 1;

    gfx::CommandList commands;
    if (!gfx_succeeded(device.begin_commands(commands), application, "begin commands"))
        return 1;
    commands.begin_rendering(target, {0.01f, 0.01f, 0.033f, 1.0f});
    commands.bind_pipeline(pipeline);
    commands.draw(nullptr, 3);
    commands.end_rendering();
    commands.barrier(gfx::Stage::color_output,
                     gfx::Access::color_write,
                     gfx::Stage::transfer,
                     gfx::Access::transfer_read);
    commands.copy_texture_to_memory(
        target, {readback_memory.gpu_ptr, readback_memory.size});
    commands.barrier(gfx::Stage::transfer,
                     gfx::Access::transfer_write,
                     gfx::Stage::host,
                     gfx::Access::host_read);
    if (!gfx_succeeded(device.submit_and_wait(std::move(commands)),
                       application,
                       "submit commands"))
        return 1;

    const auto* pixels = static_cast<const std::byte*>(readback_memory.cpu_ptr);
    bool displayed = false;
    if (argc == 1 && example_window_supported)
    {
        ExampleWindow window;
        if (!open_example_window(window, "clean_gfx triangle", width, height))
        {
            std::cerr << application << ": could not open window\n";
            return 1;
        }
        displayed = true;
        while (pump_example_window(window))
        {
            if (!present_bgra8(window, pixels, width, height))
            {
                std::cerr << application << ": could not present image\n";
                close_example_window(window);
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        close_example_window(window);
    }

    if (!displayed)
    {
        const char* output_path = argc > 1 ? argv[1] : "clean_gfx_triangle.ppm";
        if (!write_bgra8_ppm(application, output_path, pixels, width, height))
            return 1;
        std::cout << "Wrote " << output_path << '\n';
    }

    device.gpu_free(readback_memory);
    return 0;
}
