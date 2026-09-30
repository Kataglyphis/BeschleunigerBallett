// Mirrored in common/push_constants.slang; the host range is self-consistent, so drift raises no validation error.

#include <gtest/gtest.h>

#include <cstddef>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "renderer/pushConstants/PushConstantPathTracing.hpp"
#include "renderer/pushConstants/PushConstantPost.hpp"
#include "renderer/pushConstants/PushConstantRasterizer.hpp"
#include "renderer/pushConstants/PushConstantRayTracing.hpp"
#include "shared/scene/ObjMaterial.hpp"

namespace {
using Kataglyphis::VulkanRendererInternals::PushConstantPathTracing;
using Kataglyphis::VulkanRendererInternals::PushConstantPost;
using Kataglyphis::VulkanRendererInternals::PushConstantRasterizer;
using Kataglyphis::VulkanRendererInternals::PushConstantRaytracing;
}// namespace

TEST(PushConstantRasterizerUnit, ModelMatrixComesFirstAtOffsetZero)
{
    // A field inserted before model would shift every draw's transform.
    EXPECT_EQ(offsetof(PushConstantRasterizer, model), 0U);
    EXPECT_EQ(sizeof(glm::mat4), 64U);
}

TEST(PushConstantRasterizerUnit, ObjectIndexFollowsTheMatrixAndIsCovered)
{
    // model (64 bytes), then invModelRows[3] (48 bytes), then objectIndex.
    EXPECT_EQ(offsetof(PushConstantRasterizer, objectIndex), 112U);

    // The layout pushes sizeof(PushConstantRasterizer) bytes, so objectIndex must fit inside it.
    EXPECT_GE(sizeof(PushConstantRasterizer), offsetof(PushConstantRasterizer, objectIndex) + sizeof(unsigned int));
}

TEST(PushConstantRasterizerUnit, FitsTheGuaranteedPushConstantBudget)
{
    // Vulkan guarantees only 128 bytes; more works on desktop GPUs and fails on minimal ones.
    EXPECT_LE(sizeof(PushConstantRasterizer), 128U);
}

TEST(PushConstantRasterizerUnit, CarriesTheValuesItIsGiven)
{
    PushConstantRasterizer push{};
    push.model = glm::scale(glm::mat4(1.0F), glm::vec3(3.0F));
    push.objectIndex = 7U;

    EXPECT_EQ(push.objectIndex, 7U);
    EXPECT_FLOAT_EQ(push.model[0][0], 3.0F);
    EXPECT_NE(push.model, glm::mat4(1.0F));
}

TEST(PushConstantRasterizerUnit, InvModelRowsRoundTripANonUniformScale)
{
    // GLM is column-major, so row i is (m[0][i], m[1][i], m[2][i], 0).
    const glm::mat4 model = glm::scale(glm::mat4(1.0F), glm::vec3(1.0F, 2.0F, 3.0F));
    const glm::mat4 inv_transpose_model = glm::inverse(glm::transpose(model));

    PushConstantRasterizer push{};
    for (int row = 0; row < 3; ++row) {
        push.invModelRows[row] =
          glm::vec4(inv_transpose_model[0][row], inv_transpose_model[1][row], inv_transpose_model[2][row], 0.0F);
    }

    // A diagonal matrix is its own transpose, so the inverse-transpose is diag(1, 1/2, 1/3).
    EXPECT_FLOAT_EQ(push.invModelRows[0][0], 1.0F);
    EXPECT_FLOAT_EQ(push.invModelRows[1][1], 0.5F);
    EXPECT_FLOAT_EQ(push.invModelRows[2][2], 1.0F / 3.0F);

    // Off-diagonal entries stay zero and w is always zero (direction, not point).
    EXPECT_FLOAT_EQ(push.invModelRows[0][1], 0.0F);
    EXPECT_FLOAT_EQ(push.invModelRows[0][2], 0.0F);
    EXPECT_FLOAT_EQ(push.invModelRows[0][3], 0.0F);
    EXPECT_FLOAT_EQ(push.invModelRows[1][3], 0.0F);
    EXPECT_FLOAT_EQ(push.invModelRows[2][3], 0.0F);
}

// Mirrored in path_tracing/path_tracing.slang; a reordered field desyncs the two silently.
TEST(PushConstantPathTracingUnit, MatchesTheSlangTwinLayout)
{
    EXPECT_EQ(offsetof(PushConstantPathTracing, clearColor), 0U);
    EXPECT_EQ(offsetof(PushConstantPathTracing, width), 16U);
    EXPECT_EQ(offsetof(PushConstantPathTracing, height), 20U);
    EXPECT_EQ(offsetof(PushConstantPathTracing, frame_index), 24U);
    EXPECT_EQ(offsetof(PushConstantPathTracing, samples_per_pixel), 28U);
    EXPECT_EQ(offsetof(PushConstantPathTracing, max_bounces), 32U);
    EXPECT_GE(sizeof(PushConstantPathTracing), 36U);
    EXPECT_LE(sizeof(PushConstantPathTracing), 128U);
}

// Mirrored in post/post.slang.
TEST(PushConstantPostUnit, MatchesTheSlangTwinLayout)
{
    EXPECT_EQ(offsetof(PushConstantPost, clouds_enabled), 0U);
    EXPECT_EQ(sizeof(PushConstantPost), 4U);
}

// Mirrored in raytracing/rt_types.slang.
TEST(PushConstantRaytracingUnit, MatchesTheSlangTwinLayout)
{
    EXPECT_EQ(offsetof(PushConstantRaytracing, clear_color), 0U);
    EXPECT_EQ(sizeof(PushConstantRaytracing), 16U);
}

// A failure is a real C++/Slang layout finding against common/scene_types.slang; never adjust the numbers.
TEST(ObjMaterialLayoutUnit, MatchesTheSlangTwinScalarLayout)
{
    EXPECT_EQ(offsetof(ObjMaterial, diffuse), 0U);
    EXPECT_EQ(offsetof(ObjMaterial, emission), 12U);
    EXPECT_EQ(offsetof(ObjMaterial, shininess), 24U);
    EXPECT_EQ(offsetof(ObjMaterial, dissolve), 28U);
    EXPECT_EQ(offsetof(ObjMaterial, textureID), 32U);
    EXPECT_EQ(offsetof(ObjMaterial, alphaCutoff), 36U);
    EXPECT_EQ(offsetof(ObjMaterial, uv_transform_row0), 40U);
    EXPECT_EQ(offsetof(ObjMaterial, uv_transform_row1), 52U);
    EXPECT_EQ(offsetof(ObjMaterial, metallic), 64U);
    EXPECT_EQ(offsetof(ObjMaterial, roughness), 68U);
    EXPECT_EQ(offsetof(ObjMaterial, emissiveTextureID), 72U);
    EXPECT_EQ(offsetof(ObjMaterial, normalTextureID), 76U);
    EXPECT_EQ(offsetof(ObjMaterial, normalScale), 80U);
    EXPECT_EQ(offsetof(ObjMaterial, metallicRoughnessTextureID), 84U);
    EXPECT_EQ(offsetof(ObjMaterial, normal_uv_transform_row0), 88U);
    EXPECT_EQ(offsetof(ObjMaterial, normal_uv_transform_row1), 100U);
    EXPECT_EQ(offsetof(ObjMaterial, metallic_roughness_uv_transform_row0), 112U);
    EXPECT_EQ(offsetof(ObjMaterial, metallic_roughness_uv_transform_row1), 124U);
    EXPECT_EQ(offsetof(ObjMaterial, emissive_uv_transform_row0), 136U);
    EXPECT_EQ(offsetof(ObjMaterial, emissive_uv_transform_row1), 148U);
    EXPECT_EQ(offsetof(ObjMaterial, unlit), 160U);
    EXPECT_EQ(offsetof(ObjMaterial, alphaTextureID), 164U);
    EXPECT_EQ(sizeof(ObjMaterial), 168U);
}

// ObjMaterial is an aggregate, so only the member initializers carry its documented sentinels.
TEST(ObjMaterialLayoutUnit, ValueInitializedMaterialCarriesTheDocumentedSentinels)
{
    const ObjMaterial m{};

    EXPECT_EQ(m.diffuse, glm::vec3(0.7F));
    EXPECT_EQ(m.emission, glm::vec3(0.0F));
    EXPECT_FLOAT_EQ(m.shininess, 0.0F);
    EXPECT_FLOAT_EQ(m.dissolve, 1.0F);
    EXPECT_EQ(m.textureID, -1);
    EXPECT_FLOAT_EQ(m.alphaCutoff, -1.0F);
    EXPECT_EQ(m.uv_transform_row0, glm::vec3(1.0F, 0.0F, 0.0F));
    EXPECT_EQ(m.uv_transform_row1, glm::vec3(0.0F, 1.0F, 0.0F));
    EXPECT_FLOAT_EQ(m.metallic, 0.0F);
    EXPECT_FLOAT_EQ(m.roughness, -1.0F);
    EXPECT_EQ(m.emissiveTextureID, -1);
    EXPECT_EQ(m.normalTextureID, -1);
    EXPECT_FLOAT_EQ(m.normalScale, 1.0F);
    EXPECT_EQ(m.metallicRoughnessTextureID, -1);
    EXPECT_EQ(m.normal_uv_transform_row0, glm::vec3(1.0F, 0.0F, 0.0F));
    EXPECT_EQ(m.normal_uv_transform_row1, glm::vec3(0.0F, 1.0F, 0.0F));
    EXPECT_EQ(m.metallic_roughness_uv_transform_row0, glm::vec3(1.0F, 0.0F, 0.0F));
    EXPECT_EQ(m.metallic_roughness_uv_transform_row1, glm::vec3(0.0F, 1.0F, 0.0F));
    EXPECT_EQ(m.emissive_uv_transform_row0, glm::vec3(1.0F, 0.0F, 0.0F));
    EXPECT_EQ(m.emissive_uv_transform_row1, glm::vec3(0.0F, 1.0F, 0.0F));
    EXPECT_EQ(m.unlit, 0);
    EXPECT_EQ(m.alphaTextureID, -1);
}
