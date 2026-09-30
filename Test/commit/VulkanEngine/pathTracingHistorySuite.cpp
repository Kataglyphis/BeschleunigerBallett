#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "renderer/PathTracingHistory.hpp"

using Kataglyphis::VulkanRendererInternals::PathTracingHistoryKey;

namespace {

auto make_key() -> PathTracingHistoryKey
{
    return PathTracingHistoryKey{
        .view = glm::mat4(1.0F),
        .projection = glm::mat4(1.0F),
        .lightDirection = glm::vec4(0.0F, -1.0F, 0.0F, 1.0F),
        .lightColorAndRadiance = glm::vec4(1.0F, 1.0F, 1.0F, 2.0F),
        .samplesPerPixel = 4,
        .maxBounces = 3,
    };
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
    PathTracingHistoryKey b = make_key();
    b.lightDirection = glm::vec4(1.0F, 0.0F, 0.0F, 1.0F);

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
