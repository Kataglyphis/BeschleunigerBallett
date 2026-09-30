// object_descriptions holds one entry per mesh, not per model, flattened across models.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

import kataglyphis.vulkan.object_description;

using Kataglyphis::assignTextureOffsets;
using Kataglyphis::FlattenedTextureSlot;
using Kataglyphis::meshBaseOffsets;
using Kataglyphis::planFlattenedTextureSlots;

TEST(ObjectDescriptionOffsets, EveryMeshOfAModelSharesThatModelsOffset)
{
    // Descriptions are [m0.mesh0, m0.mesh1, m0.mesh2, m1.mesh0].
    std::vector<ObjectDescription> descriptions(4);
    const std::vector<uint32_t> meshCountPerModel{ 3, 1 };
    const std::vector<uint32_t> textureCountPerModel{ 5, 2 };

    assignTextureOffsets(descriptions, meshCountPerModel, textureCountPerModel);

    EXPECT_EQ(descriptions[0].texture_offset, 0U);
    EXPECT_EQ(descriptions[1].texture_offset, 0U);
    EXPECT_EQ(descriptions[2].texture_offset, 0U);
    EXPECT_EQ(descriptions[3].texture_offset, 5U);
}

TEST(ObjectDescriptionOffsets, ASingleModelLeavesEveryOffsetAtZero)
{
    std::vector<ObjectDescription> descriptions(2);
    const std::vector<uint32_t> meshCountPerModel{ 2 };
    const std::vector<uint32_t> textureCountPerModel{ 4 };

    assignTextureOffsets(descriptions, meshCountPerModel, textureCountPerModel);

    EXPECT_EQ(descriptions[0].texture_offset, 0U);
    EXPECT_EQ(descriptions[1].texture_offset, 0U);
}

TEST(ObjectDescriptionOffsets, MeshBaseOffsetsAccumulateInModelOrder)
{
    const std::vector<uint32_t> meshCountPerModel{ 2, 3, 1 };

    const auto offsets = meshBaseOffsets(meshCountPerModel);

    const std::vector<uint32_t> expected{ 0, 2, 5 };
    EXPECT_EQ(offsets, expected);
}

TEST(ObjectDescriptionOffsets, MeshBaseOffsetsHandleEmptyAndSingleMeshModels)
{
    EXPECT_TRUE(meshBaseOffsets(std::vector<uint32_t>{}).empty());

    const std::vector<uint32_t> meshCountPerModel{ 1, 1, 1 };
    const std::vector<uint32_t> expected{ 0, 1, 2 };
    EXPECT_EQ(meshBaseOffsets(meshCountPerModel), expected);

    // A model with 0 meshes must not shift its successors' offsets.
    const std::vector<uint32_t> withEmptyModel{ 1, 0, 1 };
    const std::vector<uint32_t> expectedWithEmptyModel{ 0, 1, 1 };
    EXPECT_EQ(meshBaseOffsets(withEmptyModel), expectedWithEmptyModel);
}

TEST(ObjectDescriptionOffsets, MeshBaseOffsetsAgreeWithAssignTextureOffsets)
{
    const std::vector<uint32_t> meshCountPerModel{ 2, 1, 3 };
    const std::vector<uint32_t> textureCountPerModel{ 5, 2, 3 };

    const auto offsets = meshBaseOffsets(meshCountPerModel);

    uint32_t totalMeshes = 0;
    for (const uint32_t count : meshCountPerModel) {
        totalMeshes += count;
    }
    std::vector<ObjectDescription> descriptions(totalMeshes);
    assignTextureOffsets(descriptions, meshCountPerModel, textureCountPerModel);

    uint64_t textureOffset = 0;
    for (std::size_t m = 0; m < meshCountPerModel.size(); ++m) {
        EXPECT_EQ(descriptions[offsets[m]].texture_offset, textureOffset);
        textureOffset += textureCountPerModel[m];
    }
}

TEST(ObjectDescriptionOffsets, PlanVisitsEveryModelsTexturesInModelOrder)
{
    const std::vector<uint32_t> textureCountPerModel{ 3, 2 };

    const auto plan = planFlattenedTextureSlots(textureCountPerModel, 5U);

    const std::vector<FlattenedTextureSlot> expected{
        { .model = 0, .indexInModel = 0 },
        { .model = 0, .indexInModel = 1 },
        { .model = 0, .indexInModel = 2 },
        { .model = 1, .indexInModel = 0 },
        { .model = 1, .indexInModel = 1 },
    };
    EXPECT_EQ(plan.slots, expected);
    EXPECT_EQ(plan.requestedCount, 5U);
    EXPECT_FALSE(plan.exhausted);
}

TEST(ObjectDescriptionOffsets, PlanAgreesWithAssignTextureOffsets)
{
    // assignTextureOffsets relies on each model's first planned slot matching its stamped offset.
    const std::vector<uint32_t> meshCountPerModel{ 1, 1, 1 };
    const std::vector<uint32_t> textureCountPerModel{ 5, 2, 3 };

    std::vector<ObjectDescription> descriptions(3);
    assignTextureOffsets(descriptions, meshCountPerModel, textureCountPerModel);

    const auto plan = planFlattenedTextureSlots(textureCountPerModel, 100U);

    uint32_t slotIndex = 0;
    for (std::size_t m = 0; m < meshCountPerModel.size(); ++m) {
        EXPECT_EQ(plan.slots[slotIndex].model, static_cast<uint32_t>(m));
        EXPECT_EQ(descriptions[m].texture_offset, slotIndex);
        slotIndex += textureCountPerModel[m];
    }
}

TEST(ObjectDescriptionOffsets, ExhaustionReportsTheTotalAcrossEveryModelNotJustTheOverflowingOne)
{
    const std::vector<uint32_t> textureCountPerModel{ 4, 4, 4 };

    const auto plan = planFlattenedTextureSlots(textureCountPerModel, 5U);

    EXPECT_TRUE(plan.exhausted);
    EXPECT_EQ(plan.requestedCount, 12U);
    EXPECT_EQ(plan.slots.size(), 5U);
}

TEST(ObjectDescriptionOffsets, PaddingRepeatsTheFirstSlot)
{
    const std::vector<uint32_t> textureCountPerModel{ 2 };

    const auto plan = planFlattenedTextureSlots(textureCountPerModel, 5U);

    ASSERT_EQ(plan.slots.size(), 5U);
    for (std::size_t i = 2; i < plan.slots.size(); ++i) {
        EXPECT_EQ(plan.slots[i], plan.slots[0]);
    }
    EXPECT_FALSE(plan.exhausted);
}

TEST(ObjectDescriptionOffsets, NoTexturesProducesAnEmptyPlan)
{
    const std::vector<uint32_t> textureCountPerModel{ 0, 0 };

    const auto plan = planFlattenedTextureSlots(textureCountPerModel, 5U);

    EXPECT_TRUE(plan.slots.empty());
    EXPECT_FALSE(plan.exhausted);
}

TEST(ObjectDescriptionOffsets, MoreMeshesThanDescriptionsDoesNotOverrun)
{
    // More meshes claimed than descriptions exist; ASan catches a write past the end.
    std::vector<ObjectDescription> descriptions(2);
    const std::vector<uint32_t> meshCountPerModel{ 5 };
    const std::vector<uint32_t> textureCountPerModel{ 3 };

    assignTextureOffsets(descriptions, meshCountPerModel, textureCountPerModel);

    EXPECT_EQ(descriptions[0].texture_offset, 0U);
    EXPECT_EQ(descriptions[1].texture_offset, 0U);
}
