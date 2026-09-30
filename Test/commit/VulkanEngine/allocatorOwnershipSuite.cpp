// Handle wrappers must be move-only, or two copies release one handle; these asserts fail at compile time, not teardown.

#include <gtest/gtest.h>

#include <type_traits>
#include <utility>
#include <vector>

import kataglyphis.vulkan.allocator;

using Kataglyphis::Allocator;

static_assert(!std::is_copy_constructible_v<Allocator>, "Allocator must not be copy-constructible (double-free risk)");
static_assert(!std::is_copy_assignable_v<Allocator>, "Allocator must not be copy-assignable (double-free risk)");
static_assert(std::is_nothrow_move_constructible_v<Allocator>, "Allocator must be nothrow move-constructible");
static_assert(std::is_nothrow_move_assignable_v<Allocator>, "Allocator must be nothrow move-assignable");

TEST(AllocatorOwnership, DefaultConstructAndMoveAreClean)
{
    Allocator source;                    // owns no handle (no device required)
    Allocator moved(std::move(source));  // move-construct
    Allocator target;
    target = std::move(moved);           // move-assign
    // Unwinding the three destructors without a crash is the assertion.
    SUCCEED();
}

TEST(AllocatorOwnership, RelocatesCleanlyInAVector)
{
    // Growth move-constructs, then destroys the originals; a double-freeing wrapper would fault here.
    std::vector<Allocator> allocators;
    allocators.emplace_back();
    allocators.emplace_back();
    allocators.emplace_back();
    EXPECT_EQ(allocators.size(), 3U);
}
