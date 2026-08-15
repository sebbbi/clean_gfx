#include <clean_gfx/clean_gfx.hpp>

#include <cassert>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<clean_gfx::Device>);
static_assert(std::is_nothrow_move_constructible_v<clean_gfx::Device>);
static_assert(!std::is_copy_constructible_v<clean_gfx::Buffer>);
static_assert(std::is_nothrow_move_constructible_v<clean_gfx::Buffer>);
static_assert(std::is_copy_constructible_v<clean_gfx::BufferSlice>);

int main()
{
    const clean_gfx::BufferSlice empty;
    assert(!empty);
    assert(empty.address() == 0);
    assert(empty.size() == 0);
    return 0;
}
