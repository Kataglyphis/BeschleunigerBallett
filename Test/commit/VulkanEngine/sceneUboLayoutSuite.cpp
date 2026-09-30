// Slang lays SceneUBO out std140 but glm's vec4 is unaligned, so the host must pad; never adjust the numbers.

#include <gtest/gtest.h>

#include <cstddef>
#include <glm/glm.hpp>

#include "renderer/SceneUBO.hpp"

namespace {
using Kataglyphis::VulkanRendererInternals::SceneUBO;
}// namespace

TEST(SceneUboLayoutUnit, MatchesTheCompiledStd140Block)
{
    EXPECT_EQ(offsetof(SceneUBO, dirLight), 0U);
    EXPECT_EQ(offsetof(SceneUBO, dirLight.direction), 0U);
    EXPECT_EQ(offsetof(SceneUBO, dirLight.color), 16U);
    EXPECT_EQ(offsetof(SceneUBO, pcfRadius), 32U);
    EXPECT_EQ(offsetof(SceneUBO, cascadedShadowIntensity), 36U);
    EXPECT_EQ(offsetof(SceneUBO, numCascades), 40U);
    EXPECT_EQ(offsetof(SceneUBO, cascadeSplits), 48U);
    EXPECT_EQ(offsetof(SceneUBO, cascadeLightSpaceMatrices), 64U);
    EXPECT_EQ(offsetof(SceneUBO, view_dir), 256U);
    EXPECT_EQ(offsetof(SceneUBO, cam_pos), 272U);
    EXPECT_EQ(offsetof(SceneUBO, cloudLightMarch), 288U);
    EXPECT_EQ(offsetof(SceneUBO, cloudMeshScale), 304U);
    EXPECT_EQ(offsetof(SceneUBO, cloudMeshOffset), 320U);
    EXPECT_EQ(offsetof(SceneUBO, cloudParameters), 336U);
    EXPECT_EQ(sizeof(glm::mat4) * MAX_CASCADES, 192U);
    EXPECT_EQ(sizeof(SceneUBO), 352U);
}
