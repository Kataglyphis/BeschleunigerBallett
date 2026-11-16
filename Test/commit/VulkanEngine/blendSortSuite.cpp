// The transparent pass's draw order; the golden BlendCardsCompositeInDepthOrderFromBothSides proves the pixels.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

import kataglyphis.vulkan.mesh_draw_recorder;

namespace {

using Kataglyphis::VulkanRendererInternals::BlendDraw;
using Kataglyphis::VulkanRendererInternals::sortBackToFront;

std::vector<uint32_t> object_order(const std::vector<BlendDraw> &draws)
{
    std::vector<uint32_t> order;
    order.reserve(draws.size());
    for (const BlendDraw &draw : draws) { order.push_back(draw.objectIndex); }
    return order;
}

TEST(BlendSortUnit, FarthestDrawsFirst)
{
    std::vector<BlendDraw> draws = { { 0, 0, 0, 4.0F }, { 0, 1, 1, 25.0F }, { 1, 0, 2, 9.0F } };
    sortBackToFront(draws);
    EXPECT_EQ(object_order(draws), (std::vector<uint32_t>{ 1, 2, 0 }));
}

TEST(BlendSortUnit, EqualDistancesKeepTheSceneOrder)
{
    // A stable order, or two coplanar layers would swap from frame to frame.
    std::vector<BlendDraw> draws = { { 0, 0, 0, 9.0F }, { 0, 1, 1, 9.0F }, { 0, 2, 2, 16.0F }, { 0, 3, 3, 9.0F } };
    sortBackToFront(draws);
    EXPECT_EQ(object_order(draws), (std::vector<uint32_t>{ 2, 0, 1, 3 }));
}

TEST(BlendSortUnit, EmptyAndSingleDrawListsAreUntouched)
{
    std::vector<BlendDraw> none;
    sortBackToFront(none);
    EXPECT_TRUE(none.empty());

    std::vector<BlendDraw> one = { { 3, 4, 7, 1.0F } };
    sortBackToFront(one);
    ASSERT_EQ(one.size(), 1U);
    EXPECT_EQ(one[0].objectIndex, 7U);
}

}// namespace
