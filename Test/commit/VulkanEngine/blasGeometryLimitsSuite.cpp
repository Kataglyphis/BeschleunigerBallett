// maxVertex is the highest vertex index (count - 1), not the count, and an empty mesh must not wrap to 0xFFFFFFFF.

#include <array>
#include <gtest/gtest.h>

#include "renderer/accelerationStructures/BlasCompaction.hpp"
#include "renderer/accelerationStructures/BlasGeometryLimits.hpp"
#include "shared/scene/ObjMaterial.hpp"

static_assert(Kataglyphis::blasTriangleLimits(100, 300).maxVertex == 99,
  "blasTriangleLimits must be usable in a constant expression");

static_assert(Kataglyphis::blasGeometryFlags(false) == vk::GeometryFlagBitsKHR::eOpaque,
  "blasGeometryFlags must be usable in a constant expression");

namespace {

TEST(BlasGeometryLimitsUnit, MaxVertexIsTheHighestIndexNotTheCount)
{ EXPECT_EQ(Kataglyphis::blasTriangleLimits(100, 300).maxVertex, 99U); }

TEST(BlasGeometryLimitsUnit, AnEmptyMeshDoesNotWrapToUintMax)
{
    EXPECT_EQ(Kataglyphis::blasTriangleLimits(0, 0).maxVertex, 0U);
    EXPECT_NE(Kataglyphis::blasTriangleLimits(0, 0).maxVertex, 0xFFFFFFFFU);
}

TEST(BlasGeometryLimitsUnit, PrimitiveCountTruncatesAPartialTriangle)
{ EXPECT_EQ(Kataglyphis::blasTriangleLimits(4, 7).primitiveCount, 2U); }

namespace {
    ObjMaterial opaqueMaterial()
    {
        return ObjMaterial{ .diffuse = { 0.7F, 0.7F, 0.7F },
            .emission = { 0.0F, 0.0F, 0.0F },
            .shininess = 0.0F,
            .dissolve = 1.0F,
            .textureID = -1,
            .alphaCutoff = -1.0F };
    }

    ObjMaterial maskMaterial()
    {
        return ObjMaterial{ .diffuse = { 0.7F, 0.7F, 0.7F },
            .emission = { 0.0F, 0.0F, 0.0F },
            .shininess = 0.0F,
            .dissolve = 1.0F,
            .textureID = -1,
            .alphaCutoff = 0.5F };
    }
}// namespace

TEST(BlasGeometryLimitsUnit, MaskMaterialDropsOpaque)
{
    const std::array<ObjMaterial, 1> materials{ maskMaterial() };
    EXPECT_TRUE(Kataglyphis::blasGeometryNeedsAnyHit(materials));
    EXPECT_EQ(Kataglyphis::blasGeometryFlags(Kataglyphis::blasGeometryNeedsAnyHit(materials)), vk::GeometryFlagsKHR{});
}

TEST(BlasGeometryLimitsUnit, AllOpaqueMaterialsKeepOpaque)
{
    const std::array<ObjMaterial, 2> materials{ opaqueMaterial(), opaqueMaterial() };
    EXPECT_FALSE(Kataglyphis::blasGeometryNeedsAnyHit(materials));
    EXPECT_EQ(Kataglyphis::blasGeometryFlags(Kataglyphis::blasGeometryNeedsAnyHit(materials)),
      vk::GeometryFlagBitsKHR::eOpaque);
}

TEST(BlasGeometryLimitsUnit, ASingleMaskMaterialAmongManyOpaqueOnesDropsOpaque)
{
    const std::array<ObjMaterial, 4> materials{ opaqueMaterial(), opaqueMaterial(), maskMaterial(), opaqueMaterial() };
    EXPECT_TRUE(Kataglyphis::blasGeometryNeedsAnyHit(materials));
}

TEST(BlasCompactionUnit, RejectsEmptyAndZeroSizedResults)
{
    EXPECT_FALSE(Kataglyphis::compactedSizesAreUsable({}));

    const std::array<vk::DeviceSize, 3> zero_first = { 0, 128, 256 };
    EXPECT_FALSE(Kataglyphis::compactedSizesAreUsable(zero_first));

    const std::array<vk::DeviceSize, 3> zero_middle = { 128, 0, 256 };
    EXPECT_FALSE(Kataglyphis::compactedSizesAreUsable(zero_middle));

    const std::array<vk::DeviceSize, 3> zero_last = { 128, 256, 0 };
    EXPECT_FALSE(Kataglyphis::compactedSizesAreUsable(zero_last));

    const std::array<vk::DeviceSize, 3> all_positive = { 128, 256, 512 };
    EXPECT_TRUE(Kataglyphis::compactedSizesAreUsable(all_positive));
}

}// namespace
