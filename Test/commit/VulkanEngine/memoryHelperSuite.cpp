// The namespace-scope static_assert only compiles if align_up is genuinely constexpr.

#include <gtest/gtest.h>

#include "common/MemoryHelper.hpp"

static_assert(Kataglyphis::align_up(65, 64) == 128, "align_up must be usable in a constant expression");

namespace {

TEST(MemoryHelperUnit, AlreadyAlignedInputIsUnchanged) { EXPECT_EQ(Kataglyphis::align_up(64, 64), 64U); }

TEST(MemoryHelperUnit, PartialValueRoundsUpToNextAlignment)
{
    EXPECT_EQ(Kataglyphis::align_up(65, 64), 128U);
    EXPECT_EQ(Kataglyphis::align_up(1, 64), 64U);
}

TEST(MemoryHelperUnit, ZeroStaysZero) { EXPECT_EQ(Kataglyphis::align_up(0, 64), 0U); }

TEST(MemoryHelperUnit, AlignmentOfOneIsIdentity) { EXPECT_EQ(Kataglyphis::align_up(65, 1), 65U); }

TEST(MemoryHelperUnit, SbtSourceOffsetsArePackedAtHandleSize)
{
    constexpr uint32_t handle_size = 32;

    EXPECT_EQ(Kataglyphis::sbt_handle_source_offset(0, handle_size), 0U);
    EXPECT_EQ(Kataglyphis::sbt_handle_source_offset(1, handle_size), 32U);
    EXPECT_EQ(Kataglyphis::sbt_handle_source_offset(2, handle_size), 64U);
    EXPECT_EQ(Kataglyphis::sbt_handle_source_offset(3, handle_size), 96U);
}

TEST(MemoryHelperUnit, SbtRecordOffsetsUseTheAlignedStride)
{
    constexpr uint32_t handle_size = 32;
    constexpr uint32_t alignment = 64;
    uint32_t const handle_size_aligned = Kataglyphis::align_up(handle_size, alignment);

    EXPECT_EQ(handle_size_aligned, 64U);
    EXPECT_EQ(Kataglyphis::sbt_record_offset(0, handle_size_aligned), 0U);
    EXPECT_EQ(Kataglyphis::sbt_record_offset(1, handle_size_aligned), 64U);
}

TEST(MemoryHelperUnit, SbtOffsetsAreANoOpWhenAlignmentEqualsHandleSize)
{
    constexpr uint32_t handle_size = 32;
    uint32_t const handle_size_aligned = Kataglyphis::align_up(handle_size, handle_size);

    EXPECT_EQ(handle_size_aligned, handle_size);
    EXPECT_EQ(Kataglyphis::sbt_handle_source_offset(1, handle_size), handle_size);
    EXPECT_EQ(Kataglyphis::sbt_record_offset(1, handle_size_aligned), handle_size);
}

}// namespace
