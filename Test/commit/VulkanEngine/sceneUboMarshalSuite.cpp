#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <span>

#include <climits>
#include <limits>

#include "common/SceneUboMarshal.hpp"
#include "common/host_device_shared_vars.hpp"
#include "renderer/SceneUBO.hpp"

using Kataglyphis::clampCloudLightMarchSteps;
using Kataglyphis::clampCloudMarchSteps;
using Kataglyphis::clampCloudMeshScale;
using Kataglyphis::clampPcfRadius;
using Kataglyphis::cloudWindOffset;
using Kataglyphis::fillSceneUboAmbientMaps;
using Kataglyphis::fillSceneUboCamera;
using Kataglyphis::fillSceneUboCascades;
using Kataglyphis::fillSceneUboClouds;
using Kataglyphis::fillSceneUboDirectionalLight;
using Kataglyphis::kCloudNoisePeriod;
using Kataglyphis::kMaxCloudLightMarchSteps;
using Kataglyphis::kMaxCloudMarchSteps;
using Kataglyphis::kMinCloudDensityMultiplier;
using Kataglyphis::kMinCloudLightMarchSteps;
using Kataglyphis::kMinCloudMarchSteps;
using Kataglyphis::kMinCloudMeshExtent;
using Kataglyphis::kMinViewportFraction;
using Kataglyphis::makeViewportRemap;
using Kataglyphis::makeVulkanProjection;
using Kataglyphis::sanitizeViewportFraction;
using Kataglyphis::viewportAspectRatio;
using Kataglyphis::ViewportFraction;
using Kataglyphis::VulkanRendererInternals::SceneUBO;

static_assert(viewportAspectRatio(1920, 0, ViewportFraction{}) == 1.0F, "a zero-height extent must not divide by zero");
static_assert(viewportAspectRatio(0, 1080, ViewportFraction{}) == 1.0F, "a zero-width extent is empty too");
static_assert(clampCloudMeshScale(glm::vec3(0.0F), 0.0F).x >= kMinCloudMeshExtent
                && clampCloudMeshScale(glm::vec3(0.0F), 0.0F).w >= kMinCloudDensityMultiplier,
  "an all-zero cloud mesh scale/density must not survive the clamp");

namespace {

// The bake lights the scene only while the skybox it was baked from is shown, and only once it has baked.
TEST(SceneUboMarshalUnit, TheBakedMapsLightTheSceneOnlyUnderTheShownSkybox)
{
    SceneUBO ubo{};
    fillSceneUboAmbientMaps(ubo, true, true, 4.0F);
    EXPECT_EQ(ubo.iblParams, glm::vec4(1.0F, 4.0F, 0.0F, 0.0F));
    fillSceneUboAmbientMaps(ubo, false, true, 4.0F);
    EXPECT_EQ(ubo.iblParams.x, 0.0F) << "a switched-off skybox lights with the analytic sky";
    fillSceneUboAmbientMaps(ubo, true, false, 4.0F);
    EXPECT_EQ(ubo.iblParams.x, 0.0F) << "an unbaked environment must not be read";
    EXPECT_EQ(ubo.iblParams.y, 4.0F);
}

TEST(SceneUboMarshalUnit, ZeroHeightExtentDoesNotDivideByZero)
{
    EXPECT_FLOAT_EQ(viewportAspectRatio(1920, 0, ViewportFraction{}), 1.0F);
    EXPECT_FLOAT_EQ(viewportAspectRatio(0, 1080, ViewportFraction{}), 1.0F) << "a zero width, as OxidANT's lens";
    EXPECT_FLOAT_EQ(viewportAspectRatio(1920, 1080, ViewportFraction{}), 1920.0F / 1080.0F);
}

// The docks leave the scene a rect; its aspect, not the swapchain's, must shape the projection.
TEST(SceneUboMarshalUnit, ViewportAspectComesFromTheSceneRect)
{
    const ViewportFraction scene{ 0.25F, 0.1F, 0.75F, 0.9F };
    EXPECT_FLOAT_EQ(viewportAspectRatio(1600, 900, scene), (1600.0F * 0.5F) / (900.0F * 0.8F));
}

TEST(SceneUboMarshalUnit, ViewportSanitizeKeepsAValidRect)
{
    const ViewportFraction rect = sanitizeViewportFraction(0.2F, 0.05F, 0.8F, 0.75F);
    EXPECT_FLOAT_EQ(rect.x0, 0.2F);
    EXPECT_FLOAT_EQ(rect.y0, 0.05F);
    EXPECT_FLOAT_EQ(rect.x1, 0.8F);
    EXPECT_FLOAT_EQ(rect.y1, 0.75F);
}

TEST(SceneUboMarshalUnit, ViewportSanitizeClampsToTheImage)
{
    const ViewportFraction rect = sanitizeViewportFraction(-0.5F, -1.0F, 1.5F, 2.0F);
    EXPECT_FLOAT_EQ(rect.x0, 0.0F);
    EXPECT_FLOAT_EQ(rect.y0, 0.0F);
    EXPECT_FLOAT_EQ(rect.x1, 1.0F);
    EXPECT_FLOAT_EQ(rect.y1, 1.0F);
}

// A NaN, an inverted or a collapsed rect would put NaN or a mirrored image into every pass.
TEST(SceneUboMarshalUnit, ViewportSanitizeFallsBackToTheWholeImage)
{
    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
    const std::array<ViewportFraction, 4> broken = { ViewportFraction{ kNaN, 0.0F, kNaN, 1.0F },
        ViewportFraction{ 0.8F, 0.0F, 0.2F, 1.0F },
        ViewportFraction{ 0.5F, 0.0F, 0.5F + kMinViewportFraction * 0.5F, 1.0F },
        ViewportFraction{ 0.0F, 0.6F, 1.0F, 0.6F } };
    for (const ViewportFraction &input : broken) {
        const ViewportFraction rect = sanitizeViewportFraction(input.x0, input.y0, input.x1, input.y1);
        EXPECT_FLOAT_EQ(rect.x0, 0.0F);
        EXPECT_FLOAT_EQ(rect.y0, 0.0F);
        EXPECT_FLOAT_EQ(rect.x1, 1.0F);
        EXPECT_FLOAT_EQ(rect.y1, 1.0F);
    }
}

TEST(SceneUboMarshalUnit, WholeImageViewportRemapIsTheIdentity)
{
    const glm::mat4 remap = makeViewportRemap(ViewportFraction{});
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) { EXPECT_FLOAT_EQ(remap[col][row], col == row ? 1.0F : 0.0F); }
    }
}

// Vulkan NDC runs from -1 at the top-left; the rect's fractions are measured from the same corner.
TEST(SceneUboMarshalUnit, ViewportRemapMapsTheClipSquareOntoTheRect)
{
    const ViewportFraction rect{ 0.2F, 0.1F, 0.7F, 0.8F };
    const glm::mat4 remap = makeViewportRemap(rect);
    const auto toFraction = [&remap](float ndcX, float ndcY) {
        const glm::vec4 clip = remap * glm::vec4(ndcX, ndcY, 0.5F, 1.0F);
        return glm::vec2((clip.x / clip.w + 1.0F) * 0.5F, (clip.y / clip.w + 1.0F) * 0.5F);
    };
    const glm::vec2 topLeft = toFraction(-1.0F, -1.0F);
    const glm::vec2 bottomRight = toFraction(1.0F, 1.0F);
    const glm::vec2 centre = toFraction(0.0F, 0.0F);
    EXPECT_NEAR(topLeft.x, rect.x0, 1e-6F);
    EXPECT_NEAR(topLeft.y, rect.y0, 1e-6F);
    EXPECT_NEAR(bottomRight.x, rect.x1, 1e-6F);
    EXPECT_NEAR(bottomRight.y, rect.y1, 1e-6F);
    EXPECT_NEAR(centre.x, (rect.x0 + rect.x1) * 0.5F, 1e-6F);
    EXPECT_NEAR(centre.y, (rect.y0 + rect.y1) * 0.5F, 1e-6F);
}

// The remap must not touch depth or w, or the depth test and perspective divide would change with the docks.
TEST(SceneUboMarshalUnit, ViewportRemapKeepsDepthAndW)
{
    const glm::mat4 projection = makeVulkanProjection(60.0F, 1.5F, 0.1F, 100.0F);
    const glm::mat4 remapped = makeViewportRemap(ViewportFraction{ 0.1F, 0.2F, 0.6F, 0.9F }) * projection;
    const glm::vec4 point(1.0F, -2.0F, -10.0F, 1.0F);
    const glm::vec4 before = projection * point;
    const glm::vec4 after = remapped * point;
    EXPECT_FLOAT_EQ(after.z, before.z);
    EXPECT_FLOAT_EQ(after.w, before.w);
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
    std::array<float, CASCADE_COUNT + 2> splitDepths{};
    std::array<glm::mat4, CASCADE_COUNT + 2> viewProjMatrices{};
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

    EXPECT_EQ(written, static_cast<uint32_t>(CASCADE_COUNT));
    EXPECT_EQ(ubo.numCascades, static_cast<uint32_t>(CASCADE_COUNT));
    for (int i = 0; i < CASCADE_COUNT; ++i) { EXPECT_FLOAT_EQ(ubo.cascadeSplits[i], splitDepths[i]); }
    EXPECT_FLOAT_EQ(ubo.cascadeSplits[CASCADE_COUNT], kSentinel)
      << "no write may land past cascadeSplits[CASCADE_COUNT - 1]";
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
    std::array<float, CASCADE_COUNT + 5> splitDepths{};
    std::array<glm::mat4, CASCADE_COUNT + 5> viewProjMatrices{};

    SceneUBO ubo{};

    const uint32_t written = fillSceneUboCascades(ubo,
      std::span<const float>(splitDepths),
      std::span<const glm::mat4>(viewProjMatrices),
      /*shadowsEnabled=*/true);

    EXPECT_EQ(written, static_cast<uint32_t>(CASCADE_COUNT))
      << "both spans exceed CASCADE_COUNT; the write must still stop at CASCADE_COUNT";
    EXPECT_EQ(ubo.numCascades, static_cast<uint32_t>(CASCADE_COUNT));
}

TEST(SceneUboMarshalUnit, CloudMeshScaleNeverReachesZero)
{
    const glm::vec4 degenerate = clampCloudMeshScale(glm::vec3(0.0F, 0.0F, 0.0F), 0.0F);
    EXPECT_GE(degenerate.x, kMinCloudMeshExtent);
    EXPECT_GE(degenerate.y, kMinCloudMeshExtent);
    EXPECT_GE(degenerate.z, kMinCloudMeshExtent);
    EXPECT_GE(degenerate.w, kMinCloudDensityMultiplier);

    // The box's half extents, which the slab test scales by 1/rd, must stay positive even from an all-zero input.
    EXPECT_GT(degenerate.x * 10.0F, 0.0F);
    EXPECT_GT(degenerate.y * 10.0F, 0.0F);
    EXPECT_GT(degenerate.z * 10.0F, 0.0F);

    const glm::vec4 normal = clampCloudMeshScale(glm::vec3(630.0F, 3.15F, 630.0F), 0.63F);
    EXPECT_FLOAT_EQ(normal.x, 630.0F);
    EXPECT_FLOAT_EQ(normal.y, 3.15F);
    EXPECT_FLOAT_EQ(normal.z, 630.0F);
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
      /*powderEffect=*/true,
      /*windOffset=*/glm::vec3(77.0F, 88.0F, 99.0F));

    EXPECT_FLOAT_EQ(ubo.cloudLightMarch.x, 4.0F);
    // The shared module reads yzw as the wind offset.
    EXPECT_FLOAT_EQ(ubo.cloudLightMarch.y, 77.0F);
    EXPECT_FLOAT_EQ(ubo.cloudLightMarch.z, 88.0F);
    EXPECT_FLOAT_EQ(ubo.cloudLightMarch.w, 99.0F);

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
      /*powderEffect=*/false,
      /*windOffset=*/glm::vec3(0.0F));

    EXPECT_GE(degenerate.cloudMeshScale.x, kMinCloudMeshExtent)
      << "a zero mesh scale must still go through clampCloudMeshScale's floor";
    EXPECT_GE(degenerate.cloudMeshScale.y, kMinCloudMeshExtent);
    EXPECT_GE(degenerate.cloudMeshScale.z, kMinCloudMeshExtent);
    EXPECT_GE(degenerate.cloudMeshScale.w, kMinCloudDensityMultiplier);
    EXPECT_FLOAT_EQ(degenerate.cloudParameters.z, 0.0F)
      << "powderEffect == false must produce exactly 0.0F - volumetric_clouds.slang tests > 0.5";
}

TEST(SceneUboMarshalUnit, CloudMarchStepsNeverLeaveTheShaderClampsRange)
{
    EXPECT_EQ(clampCloudMarchSteps(kMinCloudMarchSteps - 1), kMinCloudMarchSteps)
      << "the cloud march's dt divides by num_march_steps; a below-floor count must not reach the UBO";
    EXPECT_EQ(clampCloudMarchSteps(0), kMinCloudMarchSteps);
    EXPECT_EQ(clampCloudMarchSteps(kMinCloudMarchSteps), kMinCloudMarchSteps);
    EXPECT_EQ(clampCloudMarchSteps(kMaxCloudMarchSteps), kMaxCloudMarchSteps);
    EXPECT_EQ(clampCloudMarchSteps(kMaxCloudMarchSteps + 1), kMaxCloudMarchSteps);

    EXPECT_EQ(clampCloudLightMarchSteps(kMinCloudLightMarchSteps - 1), kMinCloudLightMarchSteps)
      << "the cloud march's dtL divides by num_march_steps_to_light; a below-floor count must not reach the UBO";
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
      /*powderEffect=*/false,
      /*windOffset=*/glm::vec3(0.0F));
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
      /*powderEffect=*/false,
      /*windOffset=*/glm::vec3(0.0F));
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

// The settings hold the way towards the Sun, OxidANT's convention; the shaders read the way the light travels.
TEST(SceneUboMarshal, DirectionalLightPacksTheTravelDirectionTheAmbientAndTheRadiance)
{
    SceneUBO ubo{};
    fillSceneUboDirectionalLight(ubo, glm::vec3(3.0F, 4.0F, 0.0F), glm::vec3(0.9F, 0.8F, 0.7F), 2.5F, 0.35F);

    EXPECT_FLOAT_EQ(ubo.dirLight.direction.x, -0.6F) << "direction.xyz is the unit way the light travels";
    EXPECT_FLOAT_EQ(ubo.dirLight.direction.y, -0.8F);
    EXPECT_FLOAT_EQ(ubo.dirLight.direction.z, 0.0F);
    EXPECT_FLOAT_EQ(ubo.dirLight.direction.w, 0.35F) << "direction.w carries the ambient the raster and rchit read";

    EXPECT_FLOAT_EQ(ubo.dirLight.color.x, 0.9F);
    EXPECT_FLOAT_EQ(ubo.dirLight.color.y, 0.8F);
    EXPECT_FLOAT_EQ(ubo.dirLight.color.z, 0.7F);
    EXPECT_FLOAT_EQ(ubo.dirLight.color.w, 2.5F) << "color.w carries radiance - rasterizer.slang's fs_main reads it";

    SceneUBO zeroInput{};
    fillSceneUboDirectionalLight(zeroInput, glm::vec3(0.0F), glm::vec3(1.0F), 1.0F, 0.0F);
    EXPECT_EQ(glm::vec3(zeroInput.dirLight.direction), glm::vec3(0.0F, -1.0F, 0.0F))
      << "a Sun with no direction shines straight down, the fallback normalizedLightDirection defines";
}

}// namespace

namespace {

// The UBO a frame packs at timeSeconds with the editor's default cloud settings and the given wind.
SceneUBO cloudUboAt(glm::vec3 wind, double timeSeconds)
{
    SceneUBO ubo{};
    fillSceneUboClouds(ubo,
      glm::vec3(630.0F, 3.15F, 630.0F),
      0.63F,
      glm::vec3(-0.364F, 367.0F, -18.351F),
      0.493F,
      8,
      3,
      0.966F,
      0.034F,
      true,
      cloudWindOffset(wind, timeSeconds));
    return ubo;
}

bool sameBits(const SceneUBO &a, const SceneUBO &b) { return std::memcmp(&a, &b, sizeof(SceneUBO)) == 0; }

}// namespace

TEST(SceneUboMarshalUnit, CloudWindIsVelocityTimesTimeWrappedToTheNoisePeriod)
{
    EXPECT_EQ(cloudWindOffset(glm::vec3(4.0F, -2.0F, 0.5F), 10.0), glm::vec3(40.0F, 236.0F, 5.0F))
      << "a negative drift wraps into [0, 256), as rem_euclid does in OxidANT";
    EXPECT_EQ(cloudWindOffset(glm::vec3(4.0F, 0.0F, 1.5F), 0.0), glm::vec3(0.0F));
    // A day in, double keeps the fraction a float product would lose.
    constexpr double kDay = 86400.0;
    EXPECT_EQ(cloudWindOffset(glm::vec3(4.0F), kDay + 3.25), cloudWindOffset(glm::vec3(4.0F), 3.25));
    for (const double time : { 0.0, 1.0, 63.99, 1.0e6 + 0.37 }) {
        const glm::vec3 offset = cloudWindOffset(glm::vec3(-7.3F, 3.1F, 1000.0F), time);
        for (int axis = 0; axis < 3; ++axis) {
            EXPECT_GE(offset[axis], 0.0F) << "t=" << time;
            EXPECT_LT(offset[axis], static_cast<float>(kCloudNoisePeriod)) << "t=" << time;
        }
    }
}

TEST(SceneUboMarshalUnit, CloudWindAtTimeZeroPacksTheWindlessUboBitForBit)
{
    EXPECT_TRUE(sameBits(cloudUboAt(glm::vec3(4.0F, -2.0F, 1.5F), 0.0), cloudUboAt(glm::vec3(0.0F), 0.0)))
      << "time 0 must not move the noise, or a golden pinned there depends on the wind setting";
    EXPECT_TRUE(sameBits(cloudUboAt(glm::vec3(4.0F, -2.0F, 1.5F), 0.0), cloudUboAt(glm::vec3(0.0F), 123.0)));
}

TEST(SceneUboMarshalUnit, CloudWindRepeatsAtTheSameTimeAndAfterAWholePeriod)
{
    const glm::vec3 wind(2.0F, -4.0F, 0.5F);
    EXPECT_TRUE(sameBits(cloudUboAt(wind, 3.0), cloudUboAt(wind, 3.0)));
    EXPECT_FALSE(sameBits(cloudUboAt(wind, 3.0), cloudUboAt(wind, 0.0))) << "the wind does not move the noise";
    // 512 s carries every axis a whole number of periods (1024, -2048, 256 units): the unshifted noise again.
    EXPECT_TRUE(sameBits(cloudUboAt(wind, 512.0), cloudUboAt(wind, 0.0)));
    // A 256-unit shift on one axis is no shift: 4 units/s for 65 s lands where 1 s did.
    EXPECT_TRUE(sameBits(cloudUboAt(glm::vec3(4.0F, 0.0F, 0.0F), 65.0), cloudUboAt(glm::vec3(4.0F, 0.0F, 0.0F), 1.0)));
}
