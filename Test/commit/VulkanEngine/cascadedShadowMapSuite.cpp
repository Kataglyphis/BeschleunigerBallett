// CPU-only tests for the cascaded shadow map maths; the functions under test are free so they need no device.

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <span>
#include <vector>

import kataglyphis.vulkan.cascaded_shadow_map;

namespace {

using Kataglyphis::CascadeData;
using Kataglyphis::CascadeFitParams;
using Kataglyphis::clampCascadeCount;
using Kataglyphis::computeCascadeData;
using Kataglyphis::computeCascadeDataInto;
using Kataglyphis::makeShadowPush;
using Kataglyphis::ShadowSetBinding;
using Kataglyphis::shadowSetBinding;

constexpr uint32_t kCascades = 3;
constexpr float kFov = 45.0F;
constexpr float kAspect = 16.0F / 9.0F;
constexpr float kNear = 0.1F;
constexpr float kFar = 150.0F;

glm::mat4 default_view() { return glm::lookAt(glm::vec3(0.0F, 6.0F, 26.0F), glm::vec3(0.0F, 1.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F)); }

glm::vec3 default_light() { return glm::vec3(-0.55F, -1.0F, -0.35F); }

CascadeFitParams default_params()
{
    return CascadeFitParams{
        .cameraView = default_view(),
        .cameraFov = kFov,
        .aspect = kAspect,
        .nearPlane = kNear,
        .farPlane = kFar,
        .lightDir = default_light(),
    };
}

std::vector<CascadeData> default_cascades() { return computeCascadeData(kCascades, default_params()); }

bool is_finite(const glm::mat4 &m)
{
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(m[col][row])) { return false; }
        }
    }
    return true;
}

// Corners of the camera frustum slice between two view-space depths.
std::vector<glm::vec4> frustum_slice_corners(const glm::mat4 &view, float near_d, float far_d)
{
    const glm::mat4 proj = glm::perspective(glm::radians(kFov), kAspect, near_d, far_d);
    const glm::mat4 inv = glm::inverse(proj * view);

    std::vector<glm::vec4> corners;
    for (unsigned x = 0; x < 2; ++x) {
        for (unsigned y = 0; y < 2; ++y) {
            // Vulkan NDC depth is 0..1 (GLM_FORCE_DEPTH_ZERO_TO_ONE).
            for (unsigned z = 0; z < 2; ++z) {
                const glm::vec4 pt =
                  inv * glm::vec4((2.0F * x) - 1.0F, (2.0F * y) - 1.0F, static_cast<float>(z), 1.0F);
                corners.push_back(pt / pt.w);
            }
        }
    }
    return corners;
}

}// namespace

TEST(CascadedShadowMapUnit, ProducesRequestedNumberOfCascades)
{
    EXPECT_EQ(default_cascades().size(), kCascades);
    EXPECT_TRUE(computeCascadeData(0U, default_params()).empty());
}

TEST(CascadedShadowMapUnit, SplitDepthsIncreaseAndEndAtFarPlane)
{
    const std::vector<CascadeData> cascades = default_cascades();
    ASSERT_FALSE(cascades.empty());

    float previous = kNear;
    for (size_t i = 0; i < cascades.size(); ++i) {
        EXPECT_GT(cascades[i].splitDepth, previous) << "cascade " << i << " must extend past the previous split";
        EXPECT_LE(cascades[i].splitDepth, kFar + 1e-3F);
        previous = cascades[i].splitDepth;
    }

    // Short of the far plane, geometry past the last split is silently unshadowed.
    EXPECT_NEAR(cascades.back().splitDepth, kFar, 1e-3F);
}

TEST(CascadedShadowMapUnit, MatricesAreFiniteAndNonDegenerate)
{
    for (const CascadeData &cascade : default_cascades()) {
        EXPECT_TRUE(is_finite(cascade.viewProjMatrix)) << "NaN/inf in a cascade matrix";
        EXPECT_GT(std::abs(glm::determinant(cascade.viewProjMatrix)), 1e-12F) << "cascade matrix collapsed";
    }
}

// A fragment outside its cascade's box projects off the shadow map and is treated as lit.
TEST(CascadedShadowMapUnit, EachCascadeCoversItsOwnFrustumSlice)
{
    const std::vector<CascadeData> cascades = default_cascades();
    ASSERT_EQ(cascades.size(), kCascades);

    float slice_near = kNear;
    for (size_t i = 0; i < cascades.size(); ++i) {
        const float slice_far = cascades[i].splitDepth;
        const std::vector<glm::vec4> corners = frustum_slice_corners(default_view(), slice_near, slice_far);

        for (const glm::vec4 &corner : corners) {
            const glm::vec4 clip = cascades[i].viewProjMatrix * glm::vec4(glm::vec3(corner), 1.0F);
            ASSERT_GT(std::abs(clip.w), 1e-6F);
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;

            // The box is fitted to these corners, so float error puts them a hair outside; a mis-sized box still fails.
            constexpr float kEdge = 1e-4F;
            EXPECT_GE(ndc.x, -1.0F - kEdge) << "cascade " << i << " x out of range";
            EXPECT_LE(ndc.x, 1.0F + kEdge) << "cascade " << i << " x out of range";
            EXPECT_GE(ndc.y, -1.0F - kEdge) << "cascade " << i << " y out of range";
            EXPECT_LE(ndc.y, 1.0F + kEdge) << "cascade " << i << " y out of range";
            // Vulkan depth range, not OpenGL's [-1,1].
            EXPECT_GE(ndc.z, 0.0F - kEdge) << "cascade " << i << " depth below the light near plane";
            EXPECT_LE(ndc.z, 1.0F + kEdge) << "cascade " << i << " depth beyond the light far plane";
        }

        slice_near = slice_far;
    }
}

// shadowDistance decouples shadow range from view range, so cascades are not spent on empty far-plane space.
TEST(CascadedShadowMapUnit, ShadowDistanceClampsTheCascadeRange)
{
    constexpr float kShadowDistance = 60.0F;
    auto params = default_params();
    params.shadowDistance = kShadowDistance;
    const std::vector<CascadeData> cascades = computeCascadeData(kCascades, params);

    ASSERT_EQ(cascades.size(), kCascades);
    // Ending short of the shadow distance leaves a band that renders unshadowed.
    EXPECT_NEAR(cascades.back().splitDepth, kShadowDistance, 1e-3F);
    for (const CascadeData &cascade : cascades) { EXPECT_LE(cascade.splitDepth, kShadowDistance + 1e-3F); }
}

TEST(CascadedShadowMapUnit, ShadowDistanceBeyondTheFarPlaneIsClampedToIt)
{
    auto params = default_params();
    params.shadowDistance = kFar * 10.0F;
    const std::vector<CascadeData> cascades = computeCascadeData(kCascades, params);

    ASSERT_EQ(cascades.size(), kCascades);
    EXPECT_NEAR(cascades.back().splitDepth, kFar, 1e-3F) << "cascades must never extend past what the camera sees";
}

TEST(CascadedShadowMapUnit, ZeroShadowDistanceFallsBackToTheFarPlane)
{
    // shadowDistance 0 is the documented escape hatch: cascades span the whole far plane.
    const std::vector<CascadeData> cascades = computeCascadeData(kCascades, default_params());

    ASSERT_EQ(cascades.size(), kCascades);
    EXPECT_NEAR(cascades.back().splitDepth, kFar, 1e-3F);
}

TEST(CascadedShadowMapUnit, LambdaZeroReproducesUniformSplits)
{
    auto params = default_params();
    params.splitLambda = 0.0F;
    const std::vector<CascadeData> cascades = computeCascadeData(kCascades, params);

    ASSERT_EQ(cascades.size(), kCascades);
    for (size_t i = 0; i < cascades.size(); ++i) {
        const float p = static_cast<float>(i + 1) / static_cast<float>(kCascades);
        EXPECT_NEAR(cascades[i].splitDepth, kNear + ((kFar - kNear) * p), 1e-2F)
          << "lambda 0 must be the uniform scheme exactly, so it stays a usable baseline";
    }
}

// Higher lambda starves a distant subject of texels, which is why the default is 0.0.
TEST(CascadedShadowMapUnit, HigherLambdaPullsSplitsTowardTheCamera)
{
    auto low_params = default_params();
    low_params.splitLambda = 0.1F;
    auto high_params = default_params();
    high_params.splitLambda = 0.9F;
    const std::vector<CascadeData> low = computeCascadeData(kCascades, low_params);
    const std::vector<CascadeData> high = computeCascadeData(kCascades, high_params);

    ASSERT_EQ(low.size(), high.size());
    ASSERT_GE(low.size(), 2U);
    // Every split but the last, which is pinned to the shadow far distance.
    for (size_t i = 0; i + 1 < low.size(); ++i) {
        EXPECT_LT(high[i].splitDepth, low[i].splitDepth) << "cascade " << i << " did not tighten with lambda";
    }
    EXPECT_NEAR(low.back().splitDepth, high.back().splitDepth, 1e-3F);
}

TEST(CascadedShadowMapUnit, OutOfRangeLambdaIsClamped)
{
    auto below_params = default_params();
    below_params.splitLambda = -5.0F;
    auto uniform_params = default_params();
    uniform_params.splitLambda = 0.0F;
    auto above_params = default_params();
    above_params.splitLambda = 5.0F;
    auto logarithmic_params = default_params();
    logarithmic_params.splitLambda = 1.0F;

    const std::vector<CascadeData> below = computeCascadeData(kCascades, below_params);
    const std::vector<CascadeData> uniform = computeCascadeData(kCascades, uniform_params);
    const std::vector<CascadeData> above = computeCascadeData(kCascades, above_params);
    const std::vector<CascadeData> logarithmic = computeCascadeData(kCascades, logarithmic_params);

    ASSERT_EQ(below.size(), kCascades);
    for (size_t i = 0; i < kCascades; ++i) {
        EXPECT_NEAR(below[i].splitDepth, uniform[i].splitDepth, 1e-3F);
        EXPECT_NEAR(above[i].splitDepth, logarithmic[i].splitDepth, 1e-3F);
        EXPECT_TRUE(is_finite(below[i].viewProjMatrix));
        EXPECT_TRUE(is_finite(above[i].viewProjMatrix));
    }
}

// The view part is rigid, so box width is 2/|row0| of viewProj; over the resolution that is world units per texel.
TEST(CascadedShadowMapUnit, ShadowDistanceImprovesTexelDensityOverTheSubject)
{
    constexpr float kShadowMapRes = 2048.0F;
    // Where dinosaurs.obj sits along the view axis for the debug camera.
    constexpr float kSubjectNear = 16.2F;
    constexpr float kSubjectFar = 36.5F;

    const auto worst_density = [](const std::vector<CascadeData> &cascades) {
        float near_d = kNear;
        float worst = 0.0F;
        for (const CascadeData &cascade : cascades) {
            const bool covers_subject = !(cascade.splitDepth < kSubjectNear || near_d > kSubjectFar);
            if (covers_subject) {
                const glm::mat4 &m = cascade.viewProjMatrix;
                const float row0 = glm::length(glm::vec3(m[0][0], m[1][0], m[2][0]));
                worst = std::max(worst, (2.0F / row0) / kShadowMapRes);
            }
            near_d = cascade.splitDepth;
        }
        return worst;
    };

    auto far_plane_params = default_params();
    far_plane_params.splitLambda = 0.0F;
    // Must match GUISceneSharedVars::cascade_split_lambda, or this measures a configuration nobody runs.
    auto shadow_distance_params = default_params();
    shadow_distance_params.shadowDistance = 60.0F;
    shadow_distance_params.splitLambda = 0.0F;

    const float fitted_to_far_plane = worst_density(computeCascadeData(kCascades, far_plane_params));
    const float fitted_to_shadow_distance = worst_density(computeCascadeData(kCascades, shadow_distance_params));

    ASSERT_GT(fitted_to_far_plane, 0.0F) << "no cascade covered the subject - the test framing is wrong";
    ASSERT_GT(fitted_to_shadow_distance, 0.0F) << "no cascade covered the subject - shadow distance is too short";
    EXPECT_LT(fitted_to_shadow_distance, fitted_to_far_plane)
      << "clamping the shadow range must make the subject's cascade tighter, not looser";
}

TEST(CascadedShadowMapUnit, DegenerateLightDirectionDoesNotProduceGarbage)
{
    // Normalising a zero vector gives NaN, which would poison every matrix.
    auto params = default_params();
    params.lightDir = glm::vec3(0.0F);
    const std::vector<CascadeData> cascades = computeCascadeData(kCascades, params);

    ASSERT_EQ(cascades.size(), kCascades);
    for (const CascadeData &cascade : cascades) { EXPECT_TRUE(is_finite(cascade.viewProjMatrix)); }
}

TEST(CascadedShadowMapUnit, CascadesRespondToLightDirection)
{
    auto from_above_params = default_params();
    from_above_params.lightDir = glm::vec3(0.0F, -1.0F, 0.0F);
    auto from_the_side_params = default_params();
    from_the_side_params.lightDir = glm::vec3(-1.0F, -0.2F, 0.0F);

    const std::vector<CascadeData> from_above = computeCascadeData(kCascades, from_above_params);
    const std::vector<CascadeData> from_the_side = computeCascadeData(kCascades, from_the_side_params);

    ASSERT_EQ(from_above.size(), from_the_side.size());
    // Identical matrices mean the light direction never reaches the cascade computation.
    bool any_difference = false;
    for (size_t i = 0; i < from_above.size(); ++i) {
        if (from_above[i].viewProjMatrix != from_the_side[i].viewProjMatrix) { any_difference = true; }
    }
    EXPECT_TRUE(any_difference) << "cascade matrices ignore the light direction";
}

// An identity model matrix here leaves the depth map at its clear value, so nothing is ever occluded.
TEST(CascadedShadowMapUnit, ShadowPushCarriesTheSceneModelMatrix)
{
    const glm::mat4 scene_model = glm::scale(glm::mat4(1.0F), glm::vec3(60.0F));

    const Kataglyphis::ShadowPushConstants push = makeShadowPush(scene_model, 2U);

    EXPECT_EQ(push.model, scene_model) << "the shadow pass must use the scene's model matrix, not identity";
    EXPECT_NE(push.model, glm::mat4(1.0F)) << "a non-identity scene matrix must not collapse to identity";
    EXPECT_EQ(push.cascadeIndex, 2U);
}

// The pipeline layout puts light matrices at set 1, so both paths must bind them there.
TEST(CascadedShadowMapUnit, ShadowSetBindingKeepsLightMatricesAtSetOne)
{
    EXPECT_EQ(shadowSetBinding(true), (ShadowSetBinding{ 0, 2 }));
    EXPECT_EQ(shadowSetBinding(false), (ShadowSetBinding{ 1, 1 }));
}

namespace {
// NDC scaled so one shadow-map texel is 1.0: whole-texel box motion shows up as integer deltas.
glm::vec2 texel_space(const CascadeData &cascade, const glm::vec3 &world, uint32_t resolution)
{
    const glm::vec4 clip = cascade.viewProjMatrix * glm::vec4(world, 1.0F);
    return { (clip.x / clip.w) * (static_cast<float>(resolution) / 2.0F),
        (clip.y / clip.w) * (static_cast<float>(resolution) / 2.0F) };
}

glm::mat4 translated_view(const glm::vec3 &offset)
{
    return glm::lookAt(
      glm::vec3(0.0F, 6.0F, 26.0F) + offset, glm::vec3(0.0F, 1.0F, 0.0F) + offset, glm::vec3(0.0F, 1.0F, 0.0F));
}
}// namespace

TEST(CascadedShadowMapUnit, StabilizedCascadesShiftByWholeTexelsUnderCameraMotion)
{
    // Stabilized boxes move only in whole texels, or sub-texel camera motion makes shadow edges crawl.
    constexpr uint32_t kResolution = 2048;
    const glm::vec3 tiny_offset(0.0137F, 0.0F, 0.0F);
    const glm::vec3 probe(0.0F, 1.0F, 10.0F);

    auto stab_params_a = default_params();
    stab_params_a.cameraView = translated_view({});
    stab_params_a.shadowMapResolution = kResolution;
    auto stab_params_b = default_params();
    stab_params_b.cameraView = translated_view(tiny_offset);
    stab_params_b.shadowMapResolution = kResolution;

    const auto stab_a = computeCascadeData(kCascades, stab_params_a);
    const auto stab_b = computeCascadeData(kCascades, stab_params_b);

    const glm::vec2 delta = texel_space(stab_a[0], probe, kResolution) - texel_space(stab_b[0], probe, kResolution);
    EXPECT_NEAR(delta.x, std::round(delta.x), 5e-2F) << "x moved by a fractional texel: " << delta.x;
    EXPECT_NEAR(delta.y, std::round(delta.y), 5e-2F) << "y moved by a fractional texel: " << delta.y;

    // Control: without the legacy path's fractional shift the assertions above prove nothing.
    auto legacy_params_a = default_params();
    legacy_params_a.cameraView = translated_view({});
    auto legacy_params_b = default_params();
    legacy_params_b.cameraView = translated_view(tiny_offset);
    const auto legacy_a = computeCascadeData(kCascades, legacy_params_a);
    const auto legacy_b = computeCascadeData(kCascades, legacy_params_b);
    const glm::vec2 legacy_delta =
      texel_space(legacy_a[0], probe, kResolution) - texel_space(legacy_b[0], probe, kResolution);
    const float frac_x = std::abs(legacy_delta.x - std::round(legacy_delta.x));
    const float frac_y = std::abs(legacy_delta.y - std::round(legacy_delta.y));
    EXPECT_GT(std::max(frac_x, frac_y), 5e-2F)
      << "legacy path unexpectedly texel-aligned; the stabilized assertions are vacuous";
}

TEST(CascadedShadowMapUnit, StabilizedCascadesStayTexelAlignedOverLongCameraTravel)
{
    // texel_world must come from the padded half_extent; the error only accumulates over hundreds of grid steps.
    constexpr uint32_t kResolution = 2048;
    const glm::vec3 long_offset(3.0F, 0.0F, 0.0F);
    const glm::vec3 probe(0.0F, 1.0F, 10.0F);

    auto stab_params_a = default_params();
    stab_params_a.cameraView = translated_view({});
    stab_params_a.shadowMapResolution = kResolution;
    auto stab_params_b = default_params();
    stab_params_b.cameraView = translated_view(long_offset);
    stab_params_b.shadowMapResolution = kResolution;

    const auto stab_a = computeCascadeData(kCascades, stab_params_a);
    const auto stab_b = computeCascadeData(kCascades, stab_params_b);

    const glm::vec2 delta = texel_space(stab_a[0], probe, kResolution) - texel_space(stab_b[0], probe, kResolution);
    EXPECT_NEAR(delta.x, std::round(delta.x), 5e-2F) << "x drifted off whole-texel alignment: " << delta.x;
    EXPECT_NEAR(delta.y, std::round(delta.y), 5e-2F) << "y drifted off whole-texel alignment: " << delta.y;
}

TEST(CascadedShadowMapUnit, StabilizedBoxSizeIsInvariantUnderCameraMotion)
{
    // The box is sized from the slice's bounding radius, so camera motion must not make texel size breathe.
    constexpr uint32_t kResolution = 2048;
    const glm::mat4 view_a = translated_view({});
    const glm::mat4 view_b =
      glm::lookAt(glm::vec3(3.0F, 6.0F, 20.0F), glm::vec3(1.0F, 0.0F, -4.0F), glm::vec3(0.0F, 1.0F, 0.0F));

    auto params_a = default_params();
    params_a.cameraView = view_a;
    params_a.shadowMapResolution = kResolution;
    auto params_b = default_params();
    params_b.cameraView = view_b;
    params_b.shadowMapResolution = kResolution;

    const auto cascades_a = computeCascadeData(kCascades, params_a);
    const auto cascades_b = computeCascadeData(kCascades, params_b);

    for (uint32_t i = 0; i < kCascades; ++i) {
        // viewProj = ortho * rotation, so upper-3x3 row norms recover the ortho scales.
        const auto row_norm = [](const glm::mat4 &m, int row) {
            return glm::length(glm::vec3(m[0][row], m[1][row], m[2][row]));
        };
        EXPECT_NEAR(row_norm(cascades_a[i].viewProjMatrix, 0), row_norm(cascades_b[i].viewProjMatrix, 0), 1e-5F)
          << "cascade " << i << " x scale breathes with camera motion";
        EXPECT_NEAR(row_norm(cascades_a[i].viewProjMatrix, 1), row_norm(cascades_b[i].viewProjMatrix, 1), 1e-5F)
          << "cascade " << i << " y scale breathes with camera motion";
    }
}

// viewMask spans every cascade: exceeding maxMultiviewViewCount breaks VUID-VkSubpassDescription2-viewMask-06706.
TEST(CascadedShadowMapUnit, ClampCascadeCountRespectsBothLimits)
{
    EXPECT_EQ(clampCascadeCount(3U, 3U, 8U), 3U) << "neither limit is binding";
    EXPECT_EQ(clampCascadeCount(3U, 8U, 3U), 3U) << "neither limit is binding, order swapped";
    EXPECT_EQ(clampCascadeCount(3U, 3U, 2U), 2U) << "device limit below MAX_CASCADES must bind";
    EXPECT_EQ(clampCascadeCount(8U, 3U, 8U), 3U) << "GUI slider value above MAX_CASCADES must clamp to it";
}

TEST(CascadedShadowMapUnit, ClampCascadeCountFloorsToOne)
{
    // No real device reports 0, but a cascade count of 0 is not renderable, so pin the floor.
    EXPECT_EQ(clampCascadeCount(3U, 3U, 0U), 1U);
    EXPECT_EQ(clampCascadeCount(0U, 3U, 8U), 1U) << "a requested count of 0 must still floor to 1";
}

TEST(CascadedShadowMapUnit, StabilizedCascadesStillCoverTheirSlice)
{
    // Snapping shifts the center by up to a texel; the one-texel pad must keep every slice corner covered.
    constexpr uint32_t kResolution = 2048;
    auto params = default_params();
    params.shadowMapResolution = kResolution;
    const std::vector<CascadeData> cascades = computeCascadeData(kCascades, params);
    ASSERT_EQ(cascades.size(), kCascades);

    float slice_near = kNear;
    for (size_t i = 0; i < cascades.size(); ++i) {
        const float slice_far = cascades[i].splitDepth;
        for (const glm::vec4 &corner : frustum_slice_corners(default_view(), slice_near, slice_far)) {
            const glm::vec4 clip = cascades[i].viewProjMatrix * glm::vec4(glm::vec3(corner), 1.0F);
            ASSERT_GT(std::abs(clip.w), 1e-6F);
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            constexpr float kEdge = 1e-4F;
            EXPECT_GE(ndc.x, -1.0F - kEdge) << "cascade " << i;
            EXPECT_LE(ndc.x, 1.0F + kEdge) << "cascade " << i;
            EXPECT_GE(ndc.y, -1.0F - kEdge) << "cascade " << i;
            EXPECT_LE(ndc.y, 1.0F + kEdge) << "cascade " << i;
            EXPECT_GE(ndc.z, 0.0F - kEdge) << "cascade " << i;
            EXPECT_LE(ndc.z, 1.0F + kEdge) << "cascade " << i;
        }
        slice_near = slice_far;
    }
}

// The frame path calls the Into overload while these tests call the wrapper, so the two must agree bit for bit.
TEST(CascadedShadowMapUnit, ComputeCascadeDataIntoAgreesWithTheAllocatingOverload)
{
    constexpr uint32_t kResolution = 2048;
    // Resolution 0 is the tight-fit branch, non-zero the texel-snapped one.
    for (const uint32_t resolution : { 0U, kResolution }) {
        auto params = default_params();
        params.shadowMapResolution = resolution;
        const std::vector<CascadeData> allocating = computeCascadeData(kCascades, params);
        ASSERT_EQ(allocating.size(), kCascades);

        std::array<CascadeData, kCascades> in_place{};
        computeCascadeDataInto(in_place, kCascades, params);

        for (size_t i = 0; i < kCascades; ++i) {
            EXPECT_EQ(in_place[i].splitDepth, allocating[i].splitDepth)
              << "resolution " << resolution << ", cascade " << i << ": split depth must be bit-identical";
            for (int col = 0; col < 4; ++col) {
                for (int row = 0; row < 4; ++row) {
                    EXPECT_EQ(in_place[i].viewProjMatrix[col][row], allocating[i].viewProjMatrix[col][row])
                      << "resolution " << resolution << ", cascade " << i << ", element [" << col << "][" << row
                      << "] must be bit-identical";
                }
            }
        }
    }
}

TEST(CascadedShadowMapUnit, ComputeCascadeDataIntoRefusesAnUndersizedBuffer)
{
    // Clamping to out.size() would hand back a partly stale cascade set, so a short buffer is refused untouched.
    constexpr float kSentinelSplit = -12345.0F;
    std::array<CascadeData, kCascades - 1> too_small{};
    for (CascadeData &cascade : too_small) {
        cascade.splitDepth = kSentinelSplit;
        cascade.viewProjMatrix = glm::mat4(7.0F);
    }

    computeCascadeDataInto(too_small, kCascades, default_params());

    for (const CascadeData &cascade : too_small) {
        EXPECT_EQ(cascade.splitDepth, kSentinelSplit) << "an undersized buffer must be left completely untouched";
        EXPECT_EQ(cascade.viewProjMatrix, glm::mat4(7.0F)) << "an undersized buffer must be left completely untouched";
    }

    // A larger buffer only has its first numCascades entries touched.
    std::array<CascadeData, kCascades + 1> oversized{};
    oversized[kCascades].splitDepth = kSentinelSplit;
    computeCascadeDataInto(oversized, kCascades, default_params());
    for (size_t i = 0; i < kCascades; ++i) {
        EXPECT_GT(oversized[i].splitDepth, 0.0F) << "cascade " << i << " must have been written";
    }
    EXPECT_EQ(oversized[kCascades].splitDepth, kSentinelSplit) << "entries past numCascades must not be written";
}

TEST(CascadedShadowMapUnit, DefaultFitParamsMatchTheRetiredTrailingDefaults)
{
    // splitLambda's default must track GUISceneSharedVars::cascade_split_lambda.
    const CascadeFitParams params{};
    EXPECT_EQ(params.shadowDistance, 0.0F);
    EXPECT_EQ(params.splitLambda, 0.0F);
    EXPECT_EQ(params.shadowMapResolution, 0U);
}
