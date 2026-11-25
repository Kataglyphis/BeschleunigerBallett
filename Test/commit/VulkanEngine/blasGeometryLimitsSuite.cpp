// maxVertex is the highest vertex index (count - 1), not the count, and an empty mesh must not wrap to 0xFFFFFFFF.

#include <array>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

#include "renderer/accelerationStructures/BlasCompaction.hpp"
#include "renderer/accelerationStructures/BlasGeometryLimits.hpp"
#include "renderer/accelerationStructures/BlasReuse.hpp"
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
            .alphaCutoff = 0.5F,
            .alphaMode = ALPHA_MASK };
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

// The alpha mode decides, never the cutoff: an OPAQUE or BLEND material keeps any cutoff it carries out of any-hit.
TEST(BlasGeometryLimitsUnit, ACutoffWithoutTheMaskModeKeepsOpaque)
{
    ObjMaterial opaque = opaqueMaterial();
    opaque.alphaCutoff = 0.5F;
    ObjMaterial blend = opaqueMaterial();
    blend.alphaCutoff = 0.5F;
    blend.alphaMode = ALPHA_BLEND;
    const std::array<ObjMaterial, 2> materials{ opaque, blend };
    EXPECT_FALSE(Kataglyphis::blasGeometryNeedsAnyHit(materials));
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

TEST(BlasReuseUnit, AnAppendedModelKeepsEveryEarlierBlas)
{
    const std::vector<std::shared_ptr<int>> models = { std::make_shared<int>(0), std::make_shared<int>(1) };
    const std::vector<std::weak_ptr<int>> built = { models[0] };
    EXPECT_EQ(Kataglyphis::reusableBlasPrefix(built, models), 1U);
}

TEST(BlasReuseUnit, AReplacedModelInvalidatesItselfAndEverythingAfterIt)
{
    std::vector<std::shared_ptr<int>> models = {
        std::make_shared<int>(0), std::make_shared<int>(1), std::make_shared<int>(2)
    };
    const std::vector<std::weak_ptr<int>> built = { models[0], models[1], models[2] };
    models[1] = std::make_shared<int>(1);
    EXPECT_EQ(Kataglyphis::reusableBlasPrefix(built, models), 1U);
}

TEST(BlasReuseUnit, AReloadKeepsNothingEvenAtTheFreedModelsAddress)
{
    std::vector<std::shared_ptr<int>> models = { std::make_shared<int>(0) };
    const std::vector<std::weak_ptr<int>> built = { models[0] };
    // The address may repeat once the old model is freed; the owner may not, since the weak_ptr keeps its block.
    models.clear();
    models.push_back(std::make_shared<int>(0));
    EXPECT_EQ(Kataglyphis::reusableBlasPrefix(built, models), 0U);
}

TEST(BlasReuseUnit, BlasForModelsTheSceneDroppedAreNotKept)
{
    const std::vector<std::shared_ptr<int>> models = { std::make_shared<int>(0) };
    const std::vector<std::weak_ptr<int>> built = { models[0], std::weak_ptr<int>() };
    EXPECT_EQ(Kataglyphis::reusableBlasPrefix(built, models), 1U);
    EXPECT_EQ(Kataglyphis::reusableBlasPrefix(built, std::vector<std::shared_ptr<int>>{}), 0U);
    EXPECT_EQ(Kataglyphis::reusableBlasPrefix(std::vector<std::weak_ptr<int>>{}, models), 0U);
}

}// namespace
