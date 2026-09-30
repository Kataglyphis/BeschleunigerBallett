// A false positive costs time but a false negative silently deletes a visible object, so some tests assert survival.

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>

import kataglyphis.vulkan.frustum;

namespace {

using Kataglyphis::AABB;
using Kataglyphis::extractFrustumPlanes;
using Kataglyphis::FrustumPlanes;
using Kataglyphis::isVisible;
using Kataglyphis::transformAABB;

constexpr float kFov = 45.0F;
constexpr float kAspect = 16.0F / 9.0F;
constexpr float kNear = 0.1F;
constexpr float kFar = 100.0F;

// Camera at the origin looking down -Z, matching the engine's convention.
glm::mat4 default_view_projection()
{
    const glm::mat4 view =
      glm::lookAt(glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(0.0F, 0.0F, -1.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    const glm::mat4 projection = glm::perspective(glm::radians(kFov), kAspect, kNear, kFar);
    return projection * view;
}

AABB box_at(const glm::vec3 &centre, float halfExtent)
{
    return AABB{ centre - glm::vec3(halfExtent), centre + glm::vec3(halfExtent) };
}

// Independent eight-corner oracle, so the center/extent form is not checked against itself.
AABB transform_aabb_via_eight_corners(const glm::mat4 &model, const AABB &box)
{
    AABB out{};
    bool first = true;
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 point{
            (corner & 1) != 0 ? box.max.x : box.min.x,
            (corner & 2) != 0 ? box.max.y : box.min.y,
            (corner & 4) != 0 ? box.max.z : box.min.z,
        };
        const glm::vec3 world{ model * glm::vec4(point, 1.0F) };
        if (first) {
            out.min = world;
            out.max = world;
            first = false;
        } else {
            out.min = glm::min(out.min, world);
            out.max = glm::max(out.max, world);
        }
    }
    return out;
}

}// namespace

TEST(FrustumUnit, ExtractsSixNormalizedPlanes)
{
    const FrustumPlanes planes = extractFrustumPlanes(default_view_projection());

    ASSERT_EQ(planes.size(), 6U);
    for (size_t i = 0; i < planes.size(); ++i) {
        const glm::vec3 normal{ planes[i] };
        EXPECT_NEAR(glm::length(normal), 1.0F, 1e-4F) << "plane " << i << " is not normalized";
        EXPECT_TRUE(std::isfinite(planes[i].w)) << "plane " << i << " has a non-finite distance";
    }
}

TEST(FrustumUnit, BoxInFrontOfTheCameraIsVisible)
{
    const FrustumPlanes planes = extractFrustumPlanes(default_view_projection());
    EXPECT_TRUE(isVisible(planes, box_at({ 0.0F, 0.0F, -10.0F }, 1.0F)));
}

// Each case is a visible object a naive plane test wrongly culls.
TEST(FrustumUnit, NeverCullsGeometryTheCameraCanSee)
{
    const FrustumPlanes planes = extractFrustumPlanes(default_view_projection());

    // Straddling the near plane: partly behind the camera, partly in front.
    EXPECT_TRUE(isVisible(planes, AABB{ { -1.0F, -1.0F, -0.5F }, { 1.0F, 1.0F, 0.5F } }))
      << "a box straddling the near plane is partly visible";

    // Enclosing the whole frustum puts no corner obviously inside, yet the box is visible.
    EXPECT_TRUE(isVisible(planes, box_at({ 0.0F, 0.0F, 0.0F }, 1000.0F)))
      << "a box enclosing the camera must not be culled";

    // Just inside the far plane.
    EXPECT_TRUE(isVisible(planes, box_at({ 0.0F, 0.0F, -(kFar - 1.0F) }, 0.5F)));

    // Clipping the left edge: centre is outside, part of the box is inside.
    EXPECT_TRUE(isVisible(planes, AABB{ { -12.0F, -1.0F, -20.0F }, { -6.0F, 1.0F, -18.0F } }))
      << "a box crossing the left frustum edge is partly visible";

    // Unknown bounds are no licence to cull.
    AABB inverted{};
    inverted.min = glm::vec3(1.0F);
    inverted.max = glm::vec3(-1.0F);
    ASSERT_FALSE(inverted.isValid());
    EXPECT_TRUE(isVisible(planes, inverted)) << "unknown bounds must render, not vanish";
}

// Only a box in the sliver between row2 and the OpenGL row3 + row2 near plane tells the two apart.
TEST(FrustumUnit, NearPlaneSitsAtTheProjectionNearDistance)
{
    const FrustumPlanes planes = extractFrustumPlanes(default_view_projection());

    // Between -kNear (row2 culls) and -kNear/2 (row3 + row2 would keep it).
    const AABB inSliver{ { -0.01F, -0.01F, -0.09F }, { 0.01F, 0.01F, -0.06F } };
    EXPECT_FALSE(isVisible(planes, inSliver))
      << "a box between the accurate and OpenGL-style near planes must be culled; "
         "if this fails, extractFrustumPlanes's near plane regressed to normalizePlane(row3 + row2)";

    // Control, so the assertion above cannot pass by culling everything.
    const AABB pastNearPlane{ { -0.01F, -0.01F, -0.2F }, { 0.01F, 0.01F, -0.15F } };
    EXPECT_TRUE(isVisible(planes, pastNearPlane)) << "geometry safely past the near plane must remain visible";
}

TEST(FrustumUnit, CullsGeometryOutsideEachPlane)
{
    const FrustumPlanes planes = extractFrustumPlanes(default_view_projection());

    // These pass with either near-plane form; NearPlaneSitsAtTheProjectionNearDistance is the one that distinguishes.
    EXPECT_FALSE(isVisible(planes, box_at({ 0.0F, 0.0F, 10.0F }, 1.0F))) << "geometry behind the camera must be culled";
    EXPECT_FALSE(isVisible(planes, box_at({ 0.0F, 0.0F, 0.05F }, 0.01F)))
      << "geometry just behind the camera must be culled";
    EXPECT_FALSE(isVisible(planes, box_at({ 0.0F, 0.0F, 0.5F }, 0.2F)))
      << "geometry behind the near plane must be culled";

    // Far beyond the far plane.
    EXPECT_FALSE(isVisible(planes, box_at({ 0.0F, 0.0F, -(kFar + 50.0F) }, 1.0F)));

    // Well off each side, at a depth where the frustum is still narrow.
    EXPECT_FALSE(isVisible(planes, box_at({ -100.0F, 0.0F, -10.0F }, 1.0F))) << "far left";
    EXPECT_FALSE(isVisible(planes, box_at({ 100.0F, 0.0F, -10.0F }, 1.0F))) << "far right";
    EXPECT_FALSE(isVisible(planes, box_at({ 0.0F, -100.0F, -10.0F }, 1.0F))) << "far below";
    EXPECT_FALSE(isVisible(planes, box_at({ 0.0F, 100.0F, -10.0F }, 1.0F))) << "far above";
}

// Catches an inverted plane normal, which the "far away is culled" tests miss.
TEST(FrustumUnit, BoxSpanningTheFrustumSurvivesEveryPlane)
{
    const FrustumPlanes planes = extractFrustumPlanes(default_view_projection());

    const float halfHeight = std::tan(glm::radians(kFov) * 0.5F) * 50.0F;
    const float halfWidth = halfHeight * kAspect;
    const AABB spanning{ { -halfWidth * 0.5F, -halfHeight * 0.5F, -60.0F },
        { halfWidth * 0.5F, halfHeight * 0.5F, -40.0F } };

    EXPECT_TRUE(isVisible(planes, spanning));
}

TEST(FrustumUnit, TransformAABBCoversTheRotatedBox)
{
    const AABB unit{ glm::vec3(-1.0F), glm::vec3(1.0F) };

    // Transforming only the min/max corners would report 1.0 here instead of sqrt(2).
    const glm::mat4 rotation = glm::rotate(glm::mat4(1.0F), glm::radians(45.0F), glm::vec3(0.0F, 0.0F, 1.0F));
    const AABB rotated = transformAABB(rotation, unit);

    EXPECT_NEAR(rotated.max.x, std::sqrt(2.0F), 1e-4F);
    EXPECT_NEAR(rotated.max.y, std::sqrt(2.0F), 1e-4F);
    EXPECT_NEAR(rotated.min.x, -std::sqrt(2.0F), 1e-4F);
    EXPECT_TRUE(rotated.isValid());
}

TEST(FrustumUnit, TransformAABBHandlesTranslationAndScale)
{
    const AABB unit{ glm::vec3(-1.0F), glm::vec3(1.0F) };
    const glm::mat4 model = glm::translate(glm::mat4(1.0F), glm::vec3(10.0F, 0.0F, -5.0F))
                            * glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));

    const AABB moved = transformAABB(model, unit);

    EXPECT_NEAR(moved.min.x, 8.0F, 1e-4F);
    EXPECT_NEAR(moved.max.x, 12.0F, 1e-4F);
    EXPECT_NEAR(moved.min.z, -7.0F, 1e-4F);
    EXPECT_NEAR(moved.max.z, -3.0F, 1e-4F);
}

// The matrices cover where the two forms could disagree, including a mirror (negative scale).
TEST(FrustumUnit, TransformAabbMatchesTheEightCornerReference)
{
    const AABB box{ glm::vec3(-1.0F, -2.0F, -0.5F), glm::vec3(3.0F, 1.5F, 2.0F) };

    const std::array<glm::mat4, 6> matrices{
        glm::mat4(1.0F),
        glm::translate(glm::mat4(1.0F), glm::vec3(10.0F, -3.0F, 5.0F)),
        glm::scale(glm::mat4(1.0F), glm::vec3(2.0F)),
        glm::scale(glm::mat4(1.0F), glm::vec3(1.5F, -1.0F, 3.0F)),// non-uniform, mirrored on Y
        glm::rotate(glm::mat4(1.0F), glm::radians(37.0F), glm::normalize(glm::vec3(0.3F, 1.0F, 0.2F))),
        [] {
            glm::mat4 model(1.0F);
            model = glm::translate(model, glm::vec3(12.0F, -4.0F, 7.0F));
            model = glm::rotate(model, glm::radians(37.0F), glm::normalize(glm::vec3(0.3F, 1.0F, 0.2F)));
            model = glm::scale(model, glm::vec3(1.5F, 0.5F, 2.25F));
            return model;
        }(),
    };

    for (size_t i = 0; i < matrices.size(); ++i) {
        const AABB expected = transform_aabb_via_eight_corners(matrices[i], box);
        const AABB actual = transformAABB(matrices[i], box);

        EXPECT_NEAR(actual.min.x, expected.min.x, 1e-4F) << "matrix " << i << " min.x";
        EXPECT_NEAR(actual.min.y, expected.min.y, 1e-4F) << "matrix " << i << " min.y";
        EXPECT_NEAR(actual.min.z, expected.min.z, 1e-4F) << "matrix " << i << " min.z";
        EXPECT_NEAR(actual.max.x, expected.max.x, 1e-4F) << "matrix " << i << " max.x";
        EXPECT_NEAR(actual.max.y, expected.max.y, 1e-4F) << "matrix " << i << " max.y";
        EXPECT_NEAR(actual.max.z, expected.max.z, 1e-4F) << "matrix " << i << " max.z";
    }
}

// Breaks if a caller tests object-space bounds against world-space planes.
TEST(FrustumUnit, CullingFollowsTheModelMatrix)
{
    const FrustumPlanes planes = extractFrustumPlanes(default_view_projection());
    const AABB object{ glm::vec3(-1.0F), glm::vec3(1.0F) };

    const glm::mat4 inView = glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, -20.0F));
    const glm::mat4 wayOffLeft = glm::translate(glm::mat4(1.0F), glm::vec3(-500.0F, 0.0F, -20.0F));

    EXPECT_TRUE(isVisible(planes, transformAABB(inView, object)));
    EXPECT_FALSE(isVisible(planes, transformAABB(wayOffLeft, object)));
}

// Geometry between the light and the cascade box sits outside the near plane but still casts into the box.
TEST(FrustumUnit, ShadowCasterTestIgnoresOnlyTheNearPlane)
{
    // Ortho only: its side planes parallel the light, which is what makes dropping just the near plane sufficient.
    const glm::mat4 lightView =
      glm::lookAt(glm::vec3(0.0F, 20.0F, 0.0F), glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(0.0F, 0.0F, -1.0F));
    const glm::mat4 lightProjection = glm::ortho(-10.0F, 10.0F, -10.0F, 10.0F, 1.0F, 40.0F);
    const FrustumPlanes planes = extractFrustumPlanes(lightProjection * lightView);

    // Inside the box: both tests agree.
    const AABB inside = box_at({ 0.0F, 0.0F, 0.0F }, 1.0F);
    EXPECT_TRUE(isVisible(planes, inside));
    EXPECT_TRUE(isVisibleAsShadowCaster(planes, inside));

    // Above the near plane, still casting straight down into the box.
    const AABB betweenLightAndBox = box_at({ 0.0F, 35.0F, 0.0F }, 1.0F);
    EXPECT_FALSE(isVisible(planes, betweenLightAndBox)) << "the camera test must still reject it";
    EXPECT_TRUE(isVisibleAsShadowCaster(planes, betweenLightAndBox))
      << "a caster between the light and the cascade still casts into it";

    // Outside the side planes the shadow misses the box; past the far plane it is behind everything covered.
    EXPECT_FALSE(isVisibleAsShadowCaster(planes, box_at({ -100.0F, 0.0F, 0.0F }, 1.0F))) << "far in light X";
    EXPECT_FALSE(isVisibleAsShadowCaster(planes, box_at({ 100.0F, 0.0F, 0.0F }, 1.0F))) << "far in light X";
    EXPECT_FALSE(isVisibleAsShadowCaster(planes, box_at({ 0.0F, 0.0F, 100.0F }, 1.0F))) << "far in light Y";
    EXPECT_FALSE(isVisibleAsShadowCaster(planes, box_at({ 0.0F, -50.0F, 0.0F }, 1.0F))) << "beyond the far plane";
}

TEST(FrustumUnit, ShadowCasterTestKeepsTheOtherGuarantees)
{
    const FrustumPlanes planes = extractFrustumPlanes(default_view_projection());


    // Unknown bounds must render rather than vanish, same as isVisible.
    AABB unknown{};
    unknown.min = glm::vec3(1.0F);
    unknown.max = glm::vec3(-1.0F);
    ASSERT_FALSE(unknown.isValid());
    EXPECT_TRUE(isVisibleAsShadowCaster(planes, unknown));

    // And a box enclosing everything is still a caster.
    EXPECT_TRUE(isVisibleAsShadowCaster(planes, box_at({ 0.0F, 0.0F, 0.0F }, 1000.0F)));
}
