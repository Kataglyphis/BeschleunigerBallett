#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "renderer/PathTracingHistory.hpp"

using Kataglyphis::VulkanRendererInternals::PathTracingHistoryKey;

namespace {

using Kataglyphis::VulkanRendererInternals::DirectionalLightData;
using Kataglyphis::VulkanRendererInternals::makePathTracingHistoryKey;
using Kataglyphis::VulkanRendererInternals::PathTracingFurnace;

// The renderer's own key for `light`, so the tests hold whatever the key keeps of it.
auto key_for(const DirectionalLightData &light) -> PathTracingHistoryKey
{ return makePathTracingHistoryKey(glm::mat4(1.0F), glm::mat4(1.0F), light, 4, 3, true, PathTracingFurnace{}); }

auto make_key() -> PathTracingHistoryKey
{
    return key_for(DirectionalLightData{
      .direction = glm::vec4(0.0F, -1.0F, 0.0F, 0.35F), .color = glm::vec4(1.0F, 1.0F, 1.0F, 2.0F) });
}

}// namespace

TEST(PathTracingHistoryUnit, IdenticalKeysCompareEqual)
{
    const PathTracingHistoryKey a = make_key();
    const PathTracingHistoryKey b = make_key();

    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
}

TEST(PathTracingHistoryUnit, ALightDirectionChangeInvalidatesTheHistory)
{
    const PathTracingHistoryKey a = make_key();
    const PathTracingHistoryKey b = key_for(DirectionalLightData{
      .direction = glm::vec4(1.0F, 0.0F, 0.0F, 0.35F), .color = glm::vec4(1.0F, 1.0F, 1.0F, 2.0F) });

    EXPECT_TRUE(a != b);
}

// Radiance rides in .w, which an rgb-only comparison would drop.
TEST(PathTracingHistoryUnit, ARadianceChangeInvalidatesTheHistory)
{
    const PathTracingHistoryKey a = make_key();
    PathTracingHistoryKey b = make_key();
    b.lightColorAndRadiance.w = a.lightColorAndRadiance.w + 1.0F;

    EXPECT_TRUE(a != b);
}

TEST(PathTracingHistoryUnit, ACameraMoveStillInvalidatesTheHistory)
{
    const PathTracingHistoryKey a = make_key();
    PathTracingHistoryKey b = make_key();
    b.view = glm::translate(glm::mat4(1.0F), glm::vec3(1.0F, 0.0F, 0.0F));

    EXPECT_TRUE(a != b);
}

// Primary rays come from inv_projection, so a FOV change would blend two fields of view.
TEST(PathTracingHistoryUnit, ProjectionChangeInvalidatesTheHistory)
{
    const PathTracingHistoryKey a = make_key();
    PathTracingHistoryKey b = make_key();
    b.projection = glm::perspective(glm::radians(60.0F), 16.0F / 9.0F, 0.1F, 150.0F);

    EXPECT_TRUE(a != b);
}

// A bounce that misses sees the cube map or the navy, so the toggle changes what the mean converges to.
TEST(PathTracingHistoryUnit, ASkyboxToggleInvalidatesTheHistory)
{
    const PathTracingHistoryKey a = make_key();
    PathTracingHistoryKey b = make_key();
    b.skyboxEnabled = !a.skyboxEnabled;

    EXPECT_TRUE(a != b);
}

// The Ambient slider moves dirLight.direction.w, which scales the sky a path-traced bounce sees, so it restarts the mean.
TEST(PathTracingHistoryUnit, TheAmbientInTheLightsWInvalidatesTheHistory)
{
    const DirectionalLightData dim{ .direction = glm::vec4(0.0F, -1.0F, 0.0F, 0.35F),
        .color = glm::vec4(1.0F, 1.0F, 1.0F, 2.0F) };
    DirectionalLightData bright = dim;
    bright.direction.w = 2.0F;

    EXPECT_TRUE(key_for(dim) != key_for(bright)) << "an Ambient change kept the path-traced mean";
}

// The editor's White furnace swaps every surface and the sky, so a mean across the toggle blends two scenes.
TEST(PathTracingHistoryUnit, TheFurnaceAndItsSurfaceInvalidateTheHistory)
{
    const PathTracingHistoryKey off = make_key();
    EXPECT_FALSE(off.furnace.on) << "a key made without the furnace holds it on";
    PathTracingHistoryKey on = make_key();
    on.furnace.on = true;
    EXPECT_TRUE(off != on) << "the White furnace toggle kept the path-traced mean";

    for (const auto edit : { +[](PathTracingFurnace &furnace) { furnace.radiance = 2.0F; },
           +[](PathTracingFurnace &furnace) { furnace.metallic = 1.0F; },
           +[](PathTracingFurnace &furnace) { furnace.roughness = 0.25F; } }) {
        PathTracingHistoryKey changed = on;
        edit(changed.furnace);
        EXPECT_TRUE(on != changed) << "a furnace surface change kept the path-traced mean";
    }
}
