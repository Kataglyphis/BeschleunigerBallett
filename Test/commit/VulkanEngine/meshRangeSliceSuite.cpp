// Otherwise only reached through GPU rendering; the traps are vertexBase re-basing and half-open bounds.

#include <gtest/gtest.h>

#include <span>
#include <vector>

import kataglyphis.vulkan.mesh_range;

using Kataglyphis::MeshRange;
using Kataglyphis::meshRangeFits;
using Kataglyphis::rebaseIndices;

// The upload re-bases a range's indices straight into the staging memory, without a slice.
TEST(MeshRangeSlice, RebaseWritesTheRangesIndicesRelativeToItsFirstVertex)
{
    const std::vector<unsigned int> indices = { 0, 1, 2, 3, 4, 5, 3, 5, 4 };
    const MeshRange range{ 3, 3, 3, 6, 1, 2 };
    std::vector<unsigned int> staged(range.indexCount, 99U);

    rebaseIndices(std::span(indices).subspan(range.indexStart, range.indexCount),
      static_cast<unsigned int>(range.vertexBase),
      staged);

    const std::vector<unsigned int> expected = { 0, 1, 2, 0, 2, 1 };
    EXPECT_EQ(staged, expected);
}

TEST(MeshRangeSlice, RebaseNeverWritesPastItsOutput)
{
    const std::vector<unsigned int> indices = { 5, 6, 7 };
    std::vector<unsigned int> staged = { 99U, 99U, 42U };

    rebaseIndices(indices, 5U, std::span(staged).first(2));

    const std::vector<unsigned int> expected = { 0, 1, 42 };
    EXPECT_EQ(staged, expected);
}

// Both upload paths decide with meshRangeFits whether a range gets its mesh or an empty one.
TEST(MeshRangeSlice, ARangeFitsUpToTheEndOfEachArrayAndNotOnePast)
{
    EXPECT_TRUE(meshRangeFits(MeshRange{ 0, 3, 0, 3, 0, 1 }, 3, 3, 1));
    EXPECT_TRUE(meshRangeFits(MeshRange{ 3, 0, 3, 0, 1, 0 }, 3, 3, 1)) << "an empty range at the ends fits";
    EXPECT_FALSE(meshRangeFits(MeshRange{ 1, 3, 0, 3, 0, 1 }, 3, 3, 1)) << "vertices one past the end";
    EXPECT_FALSE(meshRangeFits(MeshRange{ 0, 3, 1, 3, 0, 1 }, 3, 3, 1)) << "indices one past the end";
    EXPECT_FALSE(meshRangeFits(MeshRange{ 0, 3, 0, 3, 1, 1 }, 3, 3, 1)) << "triangles one past the end";
    EXPECT_FALSE(meshRangeFits(MeshRange{ 4, 0, 0, 3, 0, 1 }, 3, 3, 1)) << "a base past the end must not wrap";
}
