#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <span>

#include <climits>

#include "common/SceneUboMarshal.hpp"
#include "common/host_device_shared_vars.hpp"
#include "renderer/SceneUBO.hpp"

using Kataglyphis::aspectRatioOf;
using Kataglyphis::clampCloudLightMarchSteps;
using Kataglyphis::clampCloudMarchSteps;
using Kataglyphis::clampCloudMeshScale;
using Kataglyphis::clampPcfRadius;
using Kataglyphis::fillSceneUboCamera;
using Kataglyphis::fillSceneUboCascades;
using Kataglyphis::fillSceneUboClouds;
using Kataglyphis::fillSceneUboDirectionalLight;
using Kataglyphis::kMaxCloudLightMarchSteps;
using Kataglyphis::kMaxCloudMarchSteps;
using Kataglyphis::kMinCloudDensityMultiplier;
using Kataglyphis::kMinCloudLightMarchSteps;
using Kataglyphis::kMinCloudMarchSteps;
using Kataglyphis::kMinCloudMeshExtent;
using Kataglyphis::makeVulkanProjection;
using Kataglyphis::VulkanRendererInternals::SceneUBO;

static_assert(aspectRatioOf(1920, 0) == 1.0F, "a zero-height extent must not divide by zero");
static_assert(clampCloudMeshScale(glm::vec3(0.0F), 0.0F).x >= kMinCloudMeshExtent
                && clampCloudMeshScale(glm::vec3(0.0F), 0.0F).w >= kMinCloudDensityMultiplier,
  "an all-zero cloud mesh scale/density must not survive the clamp");

namespace {

TEST(SceneUboMarshalUnit, ZeroHeightExtentDoesNotDivideByZero)
{
    EXPECT_FLOAT_EQ(aspectRatioOf(1920, 0), 1.0F);
    EXPECT_FLOAT_EQ(aspectRatioOf(1920, 1080), 1920.0F / 1080.0F);
}

TEST(SceneUboMarshalUnit, ProjectionFlipsYForVulkan)
{
    constexpr float fov = 60.0F;
    constexpr float aspect = 16.0F / 9.0F;
    constexpr float nearPlane = 0.1F;
    constexpr float farPlane = 100.0F;

    const glm::mat4 result = makeVulkanProjection(fov, aspect, nearPlane, farPlane);
    const glm::mat4 reference = glm::perspective(glm::radians(fov), aspect, nearPlane, farPlane);

    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            if (col == 1 && row == 1) {
                EXPECT_FLOAT_EQ(result[col][row], -reference[col][row]);
            } else {
                EXPECT_FLOAT_EQ(result[col][row], reference[col][row]);
            }
        }
    }
}

TEST(SceneUboMarshalUnit, CascadeFillClampsToMaxCascades)
{
    std::array<float, MAX_CASCADES + 2> splitDepths{};
    std::array<glm::mat4, MAX_CASCADES + 2> viewProjMatrices{};
    for (size_t i = 0; i < splitDepths.size(); ++i) {
        splitDepths[i] = static_cast<float>(i) + 1.0F;
        viewProjMatrices[i] = glm::mat4(static_cast<float>(i) + 1.0F);
    }

    SceneUBO ubo{};
    constexpr float kSentinel = -777.0F;
    ubo.cascadeSplits = glm::vec4(kSentinel);

    const uint32_t written = fillSceneUboCascades(ubo,
      std::span<const float>(splitDepths),
      std::span<const glm::mat4>(viewProjMatrices),
      /*shadowsEnabled=*/true);

    EXPECT_EQ(written, static_cast<uint32_t>(MAX_CASCADES));
    EXPECT_EQ(ubo.numCascades, static_cast<uint32_t>(MAX_CASCADES));
    for (int i = 0; i < MAX_CASCADES; ++i) { EXPECT_FLOAT_EQ(ubo.cascadeSplits[i], splitDepths[i]); }
    EXPECT_FLOAT_EQ(ubo.cascadeSplits[MAX_CASCADES], kSentinel)
      << "no write may land past cascadeSplits[MAX_CASCADES - 1]";
}

TEST(SceneUboMarshalUnit, ShadowsDisabledZeroesTheCascadeCountButKeepsTheMatrices)
{
    const std::array<float, 1> splitDepths{ 42.0F };
    const std::array<glm::mat4, 1> viewProjMatrices{ glm::mat4(7.0F) };

    SceneUBO ubo{};

    const uint32_t written = fillSceneUboCascades(ubo,
      std::span<const float>(splitDepths),
      std::span<const glm::mat4>(viewProjMatrices),
      /*shadowsEnabled=*/false);

    EXPECT_EQ(written, 0U);
    EXPECT_EQ(ubo.numCascades, 0U) << "shaders gate on numCascades, which must be zero while shadows are off";
    EXPECT_FLOAT_EQ(ubo.cascadeSplits[0], 42.0F) << "the matrices/splits themselves are still written";
    EXPECT_EQ(ubo.cascadeLightSpaceMatrices[0], glm::mat4(7.0F));
}

TEST(SceneUboMarshalUnit, FewerCascadesThanMaxLeavesTheRestUntouched)
{
    const std::array<float, 1> splitDepths{ 5.0F };
    const std::array<glm::mat4, 1> viewProjMatrices{ glm::mat4(1.0F) };

    SceneUBO ubo{};
    ubo.cascadeSplits = glm::vec4(-1.0F, -2.0F, -3.0F, -4.0F);
    ubo.cascadeLightSpaceMatrices[1] = glm::mat4(-2.0F);
    ubo.cascadeLightSpaceMatrices[2] = glm::mat4(-3.0F);

    fillSceneUboCascades(ubo,
      std::span<const float>(splitDepths),
      std::span<const glm::mat4>(viewProjMatrices),
      /*shadowsEnabled=*/true);

    EXPECT_FLOAT_EQ(ubo.cascadeSplits[0], 5.0F);
    EXPECT_FLOAT_EQ(ubo.cascadeSplits[1], -2.0F);
    EXPECT_FLOAT_EQ(ubo.cascadeSplits[2], -3.0F);
    EXPECT_EQ(ubo.cascadeLightSpaceMatrices[1], glm::mat4(-2.0F));
    EXPECT_EQ(ubo.cascadeLightSpaceMatrices[2], glm::mat4(-3.0F));
}

TEST(SceneUboMarshalUnit, FillCascadesTruncatesToTheShorterSpan)
{
    const std::array<float, 3> splitDepths{ 1.0F, 2.0F, 3.0F };
    const std::array<glm::mat4, 2> viewProjMatrices{ glm::mat4(1.0F), glm::mat4(2.0F) };

    SceneUBO ubo{};

    const uint32_t written = fillSceneUboCascades(ubo,
      std::span<const float>(splitDepths.data(), 2),
      std::span<const glm::mat4>(viewProjMatrices),
      /*shadowsEnabled=*/true);

    EXPECT_EQ(written, 2U) << "the shorter span (matrices) must bound the write, not the longer one (splits)";
    EXPECT_EQ(ubo.numCascades, 2U);
}

TEST(SceneUboMarshalUnit, FillCascadesNeverExceedsMaxCascades)
{
    std::array<float, MAX_CASCADES + 5> splitDepths{};
    std::array<glm::mat4, MAX_CASCADES + 5> viewProjMatrices{};

    SceneUBO ubo{};

    const uint32_t written = fillSceneUboCascades(ubo,
      std::span<const float>(splitDepths),
      std::span<const glm::mat4>(viewProjMatrices),
      /*shadowsEnabled=*/true);

    EXPECT_EQ(written, static_cast<uint32_t>(MAX_CASCADES))
      << "both spans exceed MAX_CASCADES; the write must still stop at MAX_CASCADES";
    EXPECT_EQ(ubo.numCascades, static_cast<uint32_t>(MAX_CASCADES));
}

TEST(SceneUboMarshalUnit, CloudMeshScaleNeverReachesZero)
{
    const glm::vec4 degenerate = clampCloudMeshScale(glm::vec3(0.0F, 0.0F, 0.0F), 0.0F);
    EXPECT_GE(degenerate.x, kMinCloudMeshExtent);
    EXPECT_GE(degenerate.y, kMinCloudMeshExtent);
    EXPECT_GE(degenerate.z, kMinCloudMeshExtent);
    EXPECT_GE(degenerate.w, kMinCloudDensityMultiplier);

    // clouds.slang divides by this, so it must stay positive even from an all-zero input.
    EXPECT_GT(degenerate.x * degenerate.w * 10.0F, 0.0F);
    EXPECT_GT(degenerate.y * degenerate.w * 10.0F, 0.0F);
    EXPECT_GT(degenerate.z * degenerate.w * 10.0F, 0.0F);

    const glm::vec4 normal = clampCloudMeshScale(glm::vec3(1000.0F, 5.0F, 1000.0F), 0.63F);
    EXPECT_FLOAT_EQ(normal.x, 1000.0F);
    EXPECT_FLOAT_EQ(normal.y, 5.0F);
    EXPECT_FLOAT_EQ(normal.z, 1000.0F);
    EXPECT_FLOAT_EQ(normal.w, 0.63F);
}

TEST(SceneUboMarshalUnit, CloudSlidersLandInTheDocumentedComponents)
{
    SceneUBO ubo{};
    fillSceneUboClouds(ubo,
      /*meshScale=*/glm::vec3(11.0F, 22.0F, 33.0F),
      /*densityMultiplier=*/0.5F,
      /*meshOffset=*/glm::vec3(44.0F, 55.0F, 66.0F),
      /*coverageThreshold=*/0.77F,
      /*numMarchSteps=*/8,
      /*numMarchStepsToLight=*/4,
      /*pillowness=*/0.88F,
      /*cirrusEffect=*/0.99F,
      /*powderEffect=*/true);

    EXPECT_FLOAT_EQ(ubo.cloudLightMarch.x, 4.0F);
    EXPECT_FLOAT_EQ(ubo.cloudLightMarch.y, 0.0F);
    EXPECT_FLOAT_EQ(ubo.cloudLightMarch.z, 0.0F);
    EXPECT_FLOAT_EQ(ubo.cloudLightMarch.w, 0.0F);

    EXPECT_FLOAT_EQ(ubo.cloudMeshScale.x, 11.0F);
    EXPECT_FLOAT_EQ(ubo.cloudMeshScale.y, 22.0F);
    EXPECT_FLOAT_EQ(ubo.cloudMeshScale.z, 33.0F);
    EXPECT_FLOAT_EQ(ubo.cloudMeshScale.w, 0.5F);

    EXPECT_FLOAT_EQ(ubo.cloudMeshOffset.x, 44.0F);
    EXPECT_FLOAT_EQ(ubo.cloudMeshOffset.y, 55.0F);
    EXPECT_FLOAT_EQ(ubo.cloudMeshOffset.z, 66.0F);
    EXPECT_FLOAT_EQ(ubo.cloudMeshOffset.w, 0.77F);

    EXPECT_FLOAT_EQ(ubo.cloudParameters.x, 0.88F);
    EXPECT_FLOAT_EQ(ubo.cloudParameters.y, 0.99F);
    EXPECT_FLOAT_EQ(ubo.cloudParameters.z, 1.0F);
    EXPECT_FLOAT_EQ(ubo.cloudParameters.w, 8.0F);

    SceneUBO degenerate{};
    fillSceneUboClouds(degenerate,
      /*meshScale=*/glm::vec3(0.0F),
      /*densityMultiplier=*/0.0F,
      /*meshOffset=*/glm::vec3(1.0F, 2.0F, 3.0F),
      /*coverageThreshold=*/0.1F,
      /*numMarchSteps=*/1,
      /*numMarchStepsToLight=*/1,
      /*pillowness=*/0.0F,
      /*cirrusEffect=*/0.0F,
      /*powderEffect=*/false);

    EXPECT_GE(degenerate.cloudMeshScale.x, kMinCloudMeshExtent)
      << "a zero mesh scale must still go through clampCloudMeshScale's floor";
    EXPECT_GE(degenerate.cloudMeshScale.y, kMinCloudMeshExtent);
    EXPECT_GE(degenerate.cloudMeshScale.z, kMinCloudMeshExtent);
    EXPECT_GE(degenerate.cloudMeshScale.w, kMinCloudDensityMultiplier);
    EXPECT_FLOAT_EQ(degenerate.cloudParameters.z, 0.0F)
      << "powderEffect == false must produce exactly 0.0F - clouds.slang tests > 0.5";
}

TEST(SceneUboMarshalUnit, CloudMarchStepsNeverLeaveTheShaderClampsRange)
{
    EXPECT_EQ(clampCloudMarchSteps(kMinCloudMarchSteps - 1), kMinCloudMarchSteps)
      << "clouds.slang's dt divides by num_march_steps; a below-floor count must not reach the UBO";
    EXPECT_EQ(clampCloudMarchSteps(0), kMinCloudMarchSteps);
    EXPECT_EQ(clampCloudMarchSteps(kMinCloudMarchSteps), kMinCloudMarchSteps);
    EXPECT_EQ(clampCloudMarchSteps(kMaxCloudMarchSteps), kMaxCloudMarchSteps);
    EXPECT_EQ(clampCloudMarchSteps(kMaxCloudMarchSteps + 1), kMaxCloudMarchSteps);

    EXPECT_EQ(clampCloudLightMarchSteps(kMinCloudLightMarchSteps - 1), kMinCloudLightMarchSteps)
      << "clouds.slang's dtL divides by num_march_steps_to_light; a below-floor count must not reach the UBO";
    EXPECT_EQ(clampCloudLightMarchSteps(0), kMinCloudLightMarchSteps);
    EXPECT_EQ(clampCloudLightMarchSteps(kMinCloudLightMarchSteps), kMinCloudLightMarchSteps);
    EXPECT_EQ(clampCloudLightMarchSteps(kMaxCloudLightMarchSteps), kMaxCloudLightMarchSteps);
    EXPECT_EQ(clampCloudLightMarchSteps(kMaxCloudLightMarchSteps + 1), kMaxCloudLightMarchSteps);

    SceneUBO ubo{};
    fillSceneUboClouds(ubo,
      /*meshScale=*/glm::vec3(1.0F),
      /*densityMultiplier=*/1.0F,
      /*meshOffset=*/glm::vec3(0.0F),
      /*coverageThreshold=*/0.5F,
      /*numMarchSteps=*/kMinCloudMarchSteps - 1,
      /*numMarchStepsToLight=*/kMaxCloudLightMarchSteps + 1,
      /*pillowness=*/0.0F,
      /*cirrusEffect=*/0.0F,
      /*powderEffect=*/false);
    EXPECT_FLOAT_EQ(ubo.cloudParameters.w, static_cast<float>(kMinCloudMarchSteps))
      << "fillSceneUboClouds must clamp before packing, regardless of what the GUI vars hold";
    EXPECT_FLOAT_EQ(ubo.cloudLightMarch.x, static_cast<float>(kMaxCloudLightMarchSteps));

    SceneUBO inRange{};
    fillSceneUboClouds(inRange,
      /*meshScale=*/glm::vec3(1.0F),
      /*densityMultiplier=*/1.0F,
      /*meshOffset=*/glm::vec3(0.0F),
      /*coverageThreshold=*/0.5F,
      /*numMarchSteps=*/32,
      /*numMarchStepsToLight=*/16,
      /*pillowness=*/0.0F,
      /*cirrusEffect=*/0.0F,
      /*powderEffect=*/false);
    EXPECT_FLOAT_EQ(inRange.cloudParameters.w, 32.0F) << "an in-range count must round-trip unchanged";
    EXPECT_FLOAT_EQ(inRange.cloudLightMarch.x, 16.0F);
}

TEST(SceneUboMarshalUnit, ClampPcfRadiusPinsTheBoundTheShaderLoopsOver)
{
    EXPECT_EQ(clampPcfRadius(-1), 0U) << "an unclamped cast turns -1 into 4294967295, which the shader's tap "
                                         "loop never finishes, reading as fully shadowed";
    EXPECT_EQ(clampPcfRadius(INT_MIN), 0U);
    EXPECT_EQ(clampPcfRadius(0), 0U);
    EXPECT_EQ(clampPcfRadius(MAX_PCF_RADIUS), static_cast<uint32_t>(MAX_PCF_RADIUS));
    EXPECT_EQ(clampPcfRadius(MAX_PCF_RADIUS + 1), static_cast<uint32_t>(MAX_PCF_RADIUS));
    EXPECT_EQ(clampPcfRadius(INT_MAX), static_cast<uint32_t>(MAX_PCF_RADIUS));
}

TEST(SceneUboMarshal, CameraFillsPositionAndDirection)
{
    SceneUBO ubo{};
    fillSceneUboCamera(ubo, glm::vec3(1.0F, 2.0F, 3.0F), glm::vec3(0.0F, 0.0F, -1.0F));

    EXPECT_FLOAT_EQ(ubo.cam_pos.x, 1.0F);
    EXPECT_FLOAT_EQ(ubo.cam_pos.y, 2.0F);
    EXPECT_FLOAT_EQ(ubo.cam_pos.z, 3.0F);
    EXPECT_FLOAT_EQ(ubo.cam_pos.w, 1.0F) << "cam_pos.w is filler - no shader reads past .xyz";

    EXPECT_FLOAT_EQ(ubo.view_dir.x, 0.0F);
    EXPECT_FLOAT_EQ(ubo.view_dir.y, 0.0F);
    EXPECT_FLOAT_EQ(ubo.view_dir.z, -1.0F);
    EXPECT_FLOAT_EQ(ubo.view_dir.w, 1.0F) << "view_dir.w is filler - no shader reads past .xyz";
}

TEST(SceneUboMarshal, DirectionalLightPacksRadianceInColorW)
{
    SceneUBO ubo{};
    fillSceneUboDirectionalLight(ubo, glm::vec3(3.0F, 4.0F, 0.0F), glm::vec3(0.9F, 0.8F, 0.7F), 2.5F);

    EXPECT_FLOAT_EQ(glm::length(glm::vec3(ubo.dirLight.direction)), 1.0F)
      << "direction.xyz must be unit length even for a non-normalized input";
    EXPECT_FLOAT_EQ(ubo.dirLight.direction.w, 1.0F) << "direction.w is filler - no shader reads past .xyz";

    EXPECT_FLOAT_EQ(ubo.dirLight.color.x, 0.9F);
    EXPECT_FLOAT_EQ(ubo.dirLight.color.y, 0.8F);
    EXPECT_FLOAT_EQ(ubo.dirLight.color.z, 0.7F);
    EXPECT_FLOAT_EQ(ubo.dirLight.color.w, 2.5F) << "color.w carries radiance - rasterizer.slang's fs_main reads it";

    SceneUBO zeroInput{};
    fillSceneUboDirectionalLight(zeroInput, glm::vec3(0.0F), glm::vec3(1.0F), 1.0F);
    EXPECT_FLOAT_EQ(glm::length(glm::vec3(zeroInput.dirLight.direction)), 1.0F)
      << "the zero-vector fallback normalizedLightDirection defines must still land as a unit vector";
}

}// namespace
