#pragma once

#include <clean_gfx/clean_gfx.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

struct ExampleWindow
{
    void* handle = nullptr;
    bool running = false;
};

#if defined(_WIN32)
inline constexpr bool example_window_supported = true;
#else
inline constexpr bool example_window_supported = false;
#endif

const char* gfx_error_name(gfx::Error error) noexcept;

std::vector<std::uint32_t> read_spirv(const char* application,
                                      const char* path) noexcept;
bool write_bgra8_ppm(const char* application,
                     const char* path,
                     const std::byte* bgra,
                     std::uint32_t width,
                     std::uint32_t height) noexcept;

ExampleWindow open_example_window(const char* title,
                                  std::uint32_t width,
                                  std::uint32_t height) noexcept;
bool pump_example_window(ExampleWindow& window) noexcept;
bool present_bgra8(ExampleWindow& window,
                   const std::byte* pixels,
                   std::uint32_t width,
                   std::uint32_t height) noexcept;
void close_example_window(ExampleWindow& window) noexcept;
