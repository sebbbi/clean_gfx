#include "triangle_shared.h"

#include <clean_gfx/clean_gfx.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace
{

std::vector<std::uint32_t> read_spirv(const char* path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error(std::string{"could not open shader: "} + path);
    const auto end = file.tellg();
    if (end <= 0 || static_cast<std::uint64_t>(end) % sizeof(std::uint32_t) != 0)
        throw std::runtime_error(std::string{"invalid SPIR-V file: "} + path);
    std::vector<std::uint32_t> words(
        static_cast<std::size_t>(end) / sizeof(std::uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), end);
    if (!file)
        throw std::runtime_error(std::string{"could not read shader: "} + path);
    return words;
}

void write_ppm(const char* path,
               const std::byte* rgba,
               std::uint32_t width,
               std::uint32_t height)
{
    std::ofstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error(std::string{"could not create output: "} + path);
    file << "P6\n" << width << ' ' << height << "\n255\n";
    for (std::uint64_t i = 0; i < static_cast<std::uint64_t>(width) * height; ++i)
    {
        const auto* pixel = rgba + i * 4;
        file.write(reinterpret_cast<const char*>(pixel), 3);
    }
}

template<typename T, std::size_t N>
clean_gfx::Buffer upload_array(const clean_gfx::Device& device,
                               const std::array<T, N>& values)
{
    auto buffer = device.create_buffer({
        .size = sizeof(values),
        .memory = clean_gfx::MemoryType::upload,
    });
    std::memcpy(buffer.mapped_data(), values.data(), sizeof(values));
    buffer.flush();
    return buffer;
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        constexpr std::uint32_t width = 512;
        constexpr std::uint32_t height = 512;
        const char* output_path = argc > 1 ? argv[1] : "clean_gfx_triangle.ppm";

        auto device = clean_gfx::Device::create({
            .application_name = "clean_gfx offscreen triangle",
            .texture_capacity = 1024,
            .sampler_capacity = 64,
            .enable_validation = true,
        });
        std::cout << "Using " << device.caps().device_name << '\n';

        const std::array vertices{
            Vertex{{-0.8f, -0.7f}, {0.0f, 0.0f}},
            Vertex{{0.8f, -0.7f}, {1.0f, 0.0f}},
            Vertex{{0.0f, 0.8f}, {0.5f, 1.0f}},
        };
        constexpr std::array<std::uint32_t, 3> indices{0, 1, 2};
        auto vertex_buffer = upload_array(device, vertices);
        auto index_buffer = upload_array(device, indices);

        auto sampled_texture = device.create_texture({
            .width = 2,
            .height = 2,
            .depth = 1,
            .mip_levels = 1,
            .format = clean_gfx::Format::rgba8_unorm,
            .usage = clean_gfx::TextureUsage::sampled | clean_gfx::TextureUsage::storage,
        });
        auto target = device.create_texture({
            .width = width,
            .height = height,
            .depth = 1,
            .mip_levels = 1,
            .format = clean_gfx::Format::rgba8_unorm,
            .usage = clean_gfx::TextureUsage::color_attachment |
                     clean_gfx::TextureUsage::transfer_source,
        });
        auto sampler = device.create_sampler({
            .min_filter = clean_gfx::Filter::linear,
            .mag_filter = clean_gfx::Filter::linear,
            .address_u = clean_gfx::AddressMode::clamp_to_edge,
            .address_v = clean_gfx::AddressMode::clamp_to_edge,
            .address_w = clean_gfx::AddressMode::clamp_to_edge,
        });
        auto readback = device.create_buffer({
            .size = static_cast<std::uint64_t>(width) * height * 4,
            .memory = clean_gfx::MemoryType::readback,
        });

        const auto vertex_spirv = read_spirv(CLEAN_GFX_VERTEX_SPV_PATH);
        const auto fragment_spirv = read_spirv(CLEAN_GFX_FRAGMENT_SPV_PATH);
        const auto compute_spirv = read_spirv(CLEAN_GFX_COMPUTE_SPV_PATH);
        auto pipeline = device.create_graphics_pipeline({
            .vertex_spirv = vertex_spirv,
            .fragment_spirv = fragment_spirv,
            .color_format = clean_gfx::Format::rgba8_unorm,
            .depth_enabled = false,
            .topology = clean_gfx::PrimitiveTopology::triangle_list,
            .cull = clean_gfx::CullMode::none,
        });
        auto compute_pipeline = device.create_compute_pipeline({
            .compute_spirv = compute_spirv,
        });

        const RootArguments compute_root{
            .vertices = 0,
            .texture_index = sampled_texture.storage_index(),
            .sampler_index = 0,
            .exposure = 1.0f,
            .tint_rg = {{0x3c00u}, {0x3c00u}},
            .tint_ba = {{0x3c00u}, {0x3c00u}},
            .padding = 0,
        };

        const RootArguments root{
            .vertices = vertex_buffer.address(),
            .texture_index = sampled_texture.sampled_index(),
            .sampler_index = sampler.index(),
            .exposure = 1.0f,
            .tint_rg = {{0x3c00u}, {0x3c00u}},
            .tint_ba = {{0x3c00u}, {0x3c00u}},
            .padding = 0,
        };

        auto commands = device.begin_commands();
        commands.transition(sampled_texture,
                            clean_gfx::ImageState::undefined,
                            clean_gfx::ImageState::storage);
        commands.bind_pipeline(compute_pipeline);
        commands.push_root(compute_root);
        commands.dispatch(1);
        commands.transition(sampled_texture,
                            clean_gfx::ImageState::storage,
                            clean_gfx::ImageState::shader_read);
        commands.transition(target,
                            clean_gfx::ImageState::undefined,
                            clean_gfx::ImageState::color_attachment);
        commands.begin_rendering(target, {0.03f, 0.04f, 0.08f, 1.0f});
        commands.bind_pipeline(pipeline);
        commands.push_root(root);
        commands.draw_indexed(index_buffer.slice(),
                              clean_gfx::IndexType::uint32,
                              static_cast<std::uint32_t>(indices.size()));
        commands.end_rendering();
        commands.transition(target,
                            clean_gfx::ImageState::color_attachment,
                            clean_gfx::ImageState::transfer_source);
        commands.copy_texture_to_buffer(target, readback.slice());
        commands.barrier(clean_gfx::Stage::transfer,
                         clean_gfx::Access::transfer_write,
                         clean_gfx::Stage::host,
                         clean_gfx::Access::host_read);
        device.submit_and_wait(std::move(commands));

        readback.invalidate();
        write_ppm(output_path,
                  static_cast<const std::byte*>(readback.mapped_data()),
                  width,
                  height);
        std::cout << "Wrote " << output_path << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "clean_gfx_triangle: " << error.what() << '\n';
        return 1;
    }
}
