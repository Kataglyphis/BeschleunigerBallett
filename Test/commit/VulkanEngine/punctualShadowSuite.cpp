// CPU-only tests for the punctual lights' cube shadow maps: face matrices, slot assignment and caster culling.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

import kataglyphis.vulkan.frustum;
import kataglyphis.vulkan.punctual_light;
import kataglyphis.vulkan.punctual_shadow_map;

namespace {

using Kataglyphis::PunctualLight;
using Kataglyphis::PunctualLightKind;
using Kataglyphis::PunctualShadowSlot;

// Vulkan's cube-map face selection table: the face a direction picks and its (sc, tc, ma).
struct CubeLookup
{
    std::size_t face;
    float sc;
    float tc;
    float ma;
};

CubeLookup cube_lookup(const glm::vec3 &r)
{
    const glm::vec3 a = glm::abs(r);
    if (a.x >= a.y && a.x >= a.z) {
        return r.x > 0.0F ? CubeLookup{ 0, -r.z, -r.y, a.x } : CubeLookup{ 1, r.z, -r.y, a.x };
    }
    if (a.y >= a.z) { return r.y > 0.0F ? CubeLookup{ 2, r.x, r.z, a.y } : CubeLookup{ 3, r.x, -r.z, a.y }; }
    return r.z > 0.0F ? CubeLookup{ 4, r.x, -r.y, a.z } : CubeLookup{ 5, -r.x, -r.y, a.z };
}

// A sampler reads the layer and texel the pass rendered only if each face's NDC is the table's (sc, tc) / |ma|.
TEST(PunctualShadowUnit, FaceMatricesFollowTheCubeMapSelectionTable)
{
    const PunctualShadowSlot slot{ glm::vec3(1.0F, 2.0F, -3.0F), 0.05F, 40.0F };
    const std::array<glm::mat4, 6> faces = Kataglyphis::punctualShadowFaceMatrices(slot);
    const std::array<glm::vec3, 9> directions = { glm::vec3(3.0F, 0.5F, -1.0F),
        glm::vec3(-4.0F, 1.0F, 2.0F),
        glm::vec3(0.3F, 5.0F, -0.7F),
        glm::vec3(1.1F, -6.0F, 0.4F),
        glm::vec3(-0.5F, 0.9F, 2.5F),
        glm::vec3(0.8F, -1.2F, -3.0F),
        glm::vec3(10.0F, 9.0F, 0.0F),
        glm::vec3(-2.0F, -2.0F, -1.9F),
        glm::vec3(0.01F, 0.02F, 30.0F) };
    for (const glm::vec3 &d : directions) {
        const CubeLookup lookup = cube_lookup(d);
        const glm::vec4 clip = faces[lookup.face] * glm::vec4(slot.position + d, 1.0F);
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        EXPECT_NEAR(ndc.x, lookup.sc / lookup.ma, 1e-4F) << "face " << lookup.face;
        EXPECT_NEAR(ndc.y, lookup.tc / lookup.ma, 1e-4F) << "face " << lookup.face;
        EXPECT_NEAR(ndc.z, Kataglyphis::punctualShadowDepth(lookup.ma, slot.nearPlane, slot.farPlane), 1e-5F)
          << "the shader's reference depth disagrees with face " << lookup.face;
        EXPECT_GE(ndc.z, 0.0F);
        EXPECT_LE(ndc.z, 1.0F);
    }
    EXPECT_NEAR(Kataglyphis::punctualShadowDepth(slot.nearPlane, slot.nearPlane, slot.farPlane), 0.0F, 1e-6F);
    EXPECT_NEAR(Kataglyphis::punctualShadowDepth(slot.farPlane, slot.nearPlane, slot.farPlane), 1.0F, 1e-6F);
}

PunctualLight make_light(PunctualLightKind kind, glm::vec3 position, float range)
{
    PunctualLight light;
    light.kind = kind;
    light.position = position;
    light.range = range;
    light.cosOuter = 0.5F;
    return light;
}

TEST(PunctualShadowUnit, TheFirstPointAndSpotLightsGetCubesAndDirectionalOnesNone)
{
    const std::vector<PunctualLight> lights = { make_light(PunctualLightKind::Directional, glm::vec3(0.0F), 0.0F),
        make_light(PunctualLightKind::Point, glm::vec3(1.0F, 0.0F, 0.0F), 5.0F),
        make_light(PunctualLightKind::Spot, glm::vec3(0.0F, 3.0F, 0.0F), 0.0F),
        make_light(PunctualLightKind::Point, glm::vec3(2.0F, 0.0F, 0.0F), 6.0F),
        make_light(PunctualLightKind::Point, glm::vec3(3.0F, 0.0F, 0.0F), 7.0F),
        make_light(PunctualLightKind::Point, glm::vec3(4.0F, 0.0F, 0.0F), 8.0F) };
    const std::array<Kataglyphis::PlacedPunctualLights, 1> placed = { { { lights, glm::mat4(1.0F) } } };
    std::vector<glm::vec4> rows = Kataglyphis::packPunctualLights(placed);
    const std::vector<PunctualShadowSlot> slots =
      Kataglyphis::assignPunctualShadowSlots(rows, glm::vec3(-10.0F, -1.0F, -10.0F), glm::vec3(10.0F, 1.0F, 10.0F));

    ASSERT_EQ(slots.size(), Kataglyphis::MAX_SHADOWED_PUNCTUAL_LIGHTS);
    const auto shadow_row = [&](std::size_t light) { return rows[1 + light * Kataglyphis::PUNCTUAL_LIGHT_ROWS + 3]; };
    EXPECT_EQ(shadow_row(0).y, 0.0F) << "a directional light has no cube";
    for (std::size_t light = 1; light <= 4; ++light) {
        EXPECT_EQ(shadow_row(light).y, static_cast<float>(light))
          << "light " << light << " should hold slot " << light - 1;
        EXPECT_EQ(shadow_row(light).x, 0.5F) << "the slot must not clobber cos_outer";
        EXPECT_EQ(shadow_row(light).z, slots[light - 1].nearPlane);
        EXPECT_EQ(shadow_row(light).w, slots[light - 1].farPlane);
        EXPECT_LT(slots[light - 1].nearPlane, slots[light - 1].farPlane);
    }
    EXPECT_EQ(shadow_row(5).y, 0.0F) << "past MAX_SHADOWED_PUNCTUAL_LIGHTS a light stays unshadowed";
    EXPECT_EQ(slots[0].farPlane, 5.0F) << "a ranged light's cube ends at its range";
    EXPECT_EQ(slots[0].position, glm::vec3(1.0F, 0.0F, 0.0F));
    // The spot light at y 3 has no range: its far plane must reach the scene's farthest corner.
    EXPECT_GE(slots[1].farPlane, glm::length(glm::vec3(10.0F, 4.0F, 10.0F)));
}

TEST(PunctualShadowUnit, AnUnboundedLightWithoutGeometryStillGetsADepthRange)
{
    EXPECT_EQ(Kataglyphis::unboundedShadowFar(glm::vec3(0.0F), glm::vec3(1.0F), glm::vec3(-1.0F)), 100.0F);
    EXPECT_GE(Kataglyphis::unboundedShadowFar(glm::vec3(0.0F), glm::vec3(0.0F), glm::vec3(0.1F)), 1.0F);
}

TEST(PunctualShadowUnit, CastersOutsideTheLightsReachAreCulled)
{
    const glm::vec3 light(0.0F, 2.0F, 0.0F);
    EXPECT_TRUE(Kataglyphis::castsIntoPunctualShadow(
      Kataglyphis::AABB{ glm::vec3(-1.0F, 0.0F, -1.0F), glm::vec3(1.0F, 1.0F, 1.0F) }, light, 5.0F));
    EXPECT_TRUE(Kataglyphis::castsIntoPunctualShadow(
      Kataglyphis::AABB{ glm::vec3(-1.0F, 1.0F, -1.0F), glm::vec3(1.0F, 3.0F, 1.0F) }, light, 0.5F))
      << "a box around the light casts";
    EXPECT_FALSE(Kataglyphis::castsIntoPunctualShadow(
      Kataglyphis::AABB{ glm::vec3(10.0F, 0.0F, 0.0F), glm::vec3(11.0F, 1.0F, 1.0F) }, light, 5.0F));
    EXPECT_TRUE(
      Kataglyphis::castsIntoPunctualShadow(Kataglyphis::AABB{ glm::vec3(1.0F), glm::vec3(-1.0F) }, light, 5.0F))
      << "a box of unknown extent must not be culled";
}

}// namespace
