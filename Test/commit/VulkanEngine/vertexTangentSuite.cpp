#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <functional>
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <numbers>
#include <vector>

import kataglyphis.vulkan.vertex;

namespace {

// UVs follow position, so the chart is unmirrored unless flipU negates one corner's U.
std::vector<Vertex> unitQuadVertices(bool flipU = false)
{
    const glm::vec3 normal(0.0F, 0.0F, 1.0F);
    const float u1 = flipU ? -1.0F : 1.0F;
    std::vector<Vertex> vertices;
    vertices.emplace_back(glm::vec3(0.0F, 0.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(0.0F, 0.0F));
    vertices.emplace_back(glm::vec3(1.0F, 0.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(u1, 0.0F));
    vertices.emplace_back(glm::vec3(1.0F, 1.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(u1, 1.0F));
    vertices.emplace_back(glm::vec3(0.0F, 1.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(0.0F, 1.0F));
    return vertices;
}

const std::vector<unsigned int> kQuadIndices = { 0U, 1U, 2U, 0U, 2U, 3U };

}// namespace

TEST(VertexUnit, UnitQuadTangentFollowsUvGradient)
{
    // U runs with +X on an unmirrored chart, so the tangent is +X with w = +1.
    std::vector<Vertex> vertices = unitQuadVertices();
    vertex::computeTangents(vertices, kQuadIndices);

    for (const Vertex &v : vertices) {
        EXPECT_NEAR(v.tangent.x, 1.0F, 1e-4F) << "tangent should follow +U == +X";
        EXPECT_NEAR(v.tangent.y, 0.0F, 1e-4F);
        EXPECT_NEAR(v.tangent.z, 0.0F, 1e-4F);
        EXPECT_NEAR(glm::length(glm::vec3(v.tangent)), 1.0F, 1e-4F);
        EXPECT_NEAR(glm::dot(glm::vec3(v.tangent), v.normal), 0.0F, 1e-4F);
        EXPECT_EQ(v.tangent.w, 1.0F) << "an unmirrored chart must be right-handed";
    }
}

TEST(VertexUnit, TangentIsUnitAndOrthogonalOnNonAxisAlignedFixtures)
{
    // A non-axis-aligned normal closes the gap an axis-aligned quad leaves.
    const glm::vec3 normal = glm::normalize(glm::vec3(1.0F, 1.0F, 1.0F));
    std::vector<Vertex> vertices;
    vertices.emplace_back(glm::vec3(0.0F, 0.0F, 1.0F), normal, glm::vec4(1.0F), glm::vec2(0.0F, 0.0F));
    vertices.emplace_back(glm::vec3(1.0F, 0.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(1.0F, 0.0F));
    vertices.emplace_back(glm::vec3(0.0F, 1.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(0.3F, 1.0F));
    const std::vector<unsigned int> indices = { 0U, 1U, 2U };

    vertex::computeTangents(vertices, indices);

    for (const Vertex &v : vertices) {
        EXPECT_NEAR(glm::length(glm::vec3(v.tangent)), 1.0F, 1e-4F) << "|T| must be 1";
        EXPECT_NEAR(glm::dot(glm::vec3(v.tangent), v.normal), 0.0F, 1e-4F) << "dot(T, N) must be ~0";
        EXPECT_TRUE(v.tangent.w == 1.0F || v.tangent.w == -1.0F);
    }
}

TEST(VertexUnit, MirroredUvQuadProducesNegativeHandedness)
{
    // Mirrored across U, the bitangent disagrees with cross(N, T), so w flips to -1.
    std::vector<Vertex> mirrored = unitQuadVertices(/*flipU=*/true);
    vertex::computeTangents(mirrored, kQuadIndices);

    for (const Vertex &v : mirrored) { EXPECT_EQ(v.tangent.w, -1.0F) << "a mirrored UV chart must be left-handed"; }
}

TEST(VertexUnit, DegenerateUvFallsBackToAnAxisOrthogonalToTheNormalNotNan)
{
    // Identical UVs zero the gradient determinant; the fallback must still avoid NaN.
    const glm::vec3 normal(0.0F, 0.0F, 1.0F);
    std::vector<Vertex> vertices;
    vertices.emplace_back(glm::vec3(0.0F, 0.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(0.5F, 0.5F));
    vertices.emplace_back(glm::vec3(1.0F, 0.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(0.5F, 0.5F));
    vertices.emplace_back(glm::vec3(0.0F, 1.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(0.5F, 0.5F));
    const std::vector<unsigned int> indices = { 0U, 1U, 2U };

    vertex::computeTangents(vertices, indices);

    for (const Vertex &v : vertices) {
        EXPECT_FALSE(std::isnan(v.tangent.x) || std::isnan(v.tangent.y) || std::isnan(v.tangent.z));
        EXPECT_NEAR(glm::length(glm::vec3(v.tangent)), 1.0F, 1e-4F);
        EXPECT_NEAR(glm::dot(glm::vec3(v.tangent), v.normal), 0.0F, 1e-4F);
    }
}

TEST(VertexUnit, TangentBasisMapsAFlatSampleToTheGeometricNormal)
{
    // Proves normal_map.slang's mul(nTs, M) order maps a flat texel to n, computed by hand, not via glm::mat3.
    const glm::vec3 n(0.0F, 1.0F, 0.0F);
    const glm::vec3 t(1.0F, 0.0F, 0.0F);
    const float w = 1.0F;
    const glm::vec3 b = glm::cross(n, t) * w;
    const glm::vec3 nTs(0.0F, 0.0F, 1.0F);

    const glm::vec3 rightMultiplied = nTs.x * t + nTs.y * b + nTs.z * n;
    const glm::vec3 leftMultiplied(glm::dot(t, nTs), glm::dot(b, nTs), glm::dot(n, nTs));

    EXPECT_NEAR(glm::length(rightMultiplied - n), 0.0F, 1e-6F)
      << "mul(nTs, float3x3(t, b, n)) must return the geometric normal for a flat normal-map sample";
    EXPECT_GT(glm::length(leftMultiplied - n), 1e-3F)
      << "mul(float3x3(t, b, n), nTs) is the world-to-tangent transform and must NOT return the geometric normal";
}

TEST(VertexUnit, TangentsForALaterRangeLeaveEarlierVerticesUntouched)
{
    // GltfLoader's per-primitive calls: a wrong corner offset in the range-scoped accumulators touches the first quad.
    const glm::vec4 sentinel(-1.0F, -2.0F, -3.0F, -4.0F);
    std::vector<Vertex> vertices = unitQuadVertices();
    for (Vertex &v : vertices) { v.tangent = sentinel; }
    for (Vertex &v : unitQuadVertices()) { vertices.push_back(v); }

    const std::vector<unsigned int> indices = {
        // First quad: in the array but outside the range.
        0U, 1U, 2U, 0U, 2U, 3U,
        // Second quad, offset by 4 - the range computeTangents must act on.
        4U, 5U, 6U, 4U, 6U, 7U
    };

    vertex::computeTangents(vertices, indices, /*firstIndex=*/6);

    for (std::size_t i = 0; i < 4; ++i) {
        const glm::vec4 &t = vertices[i].tangent;
        EXPECT_EQ(t.x, sentinel.x) << "first quad must stay untouched";
        EXPECT_EQ(t.y, sentinel.y) << "first quad must stay untouched";
        EXPECT_EQ(t.z, sentinel.z) << "first quad must stay untouched";
        EXPECT_EQ(t.w, sentinel.w) << "first quad must stay untouched";
    }

    for (std::size_t i = 4; i < 8; ++i) {
        const Vertex &v = vertices[i];
        EXPECT_NEAR(v.tangent.x, 1.0F, 1e-4F) << "tangent should follow +U == +X";
        EXPECT_NEAR(v.tangent.y, 0.0F, 1e-4F);
        EXPECT_NEAR(v.tangent.z, 0.0F, 1e-4F);
        EXPECT_NEAR(glm::length(glm::vec3(v.tangent)), 1.0F, 1e-4F);
        EXPECT_NEAR(glm::dot(glm::vec3(v.tangent), v.normal), 0.0F, 1e-4F);
        EXPECT_EQ(v.tangent.w, 1.0F) << "an unmirrored chart must be right-handed";
    }
}

TEST(VertexUnit, SharedVertexTangentIsIndependentOfIncidenceCount)
{
    // The fan centre is shared by six triangles, and finalizing it once must equal finalizing it six times.
    const glm::vec3 normal(0.0F, 0.0F, 1.0F);
    std::vector<Vertex> vertices;
    vertices.emplace_back(glm::vec3(0.0F, 0.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(0.0F, 0.0F));

    constexpr int kSpokes = 6;
    for (int i = 0; i < kSpokes; ++i) {
        const float angle =
          (2.0F * std::numbers::pi_v<float> * static_cast<float>(i)) / static_cast<float>(kSpokes);
        const glm::vec3 pos(std::cos(angle), std::sin(angle), 0.0F);
        vertices.emplace_back(pos, normal, glm::vec4(1.0F), glm::vec2(pos.x, pos.y));
    }

    std::vector<unsigned int> indices;
    for (unsigned int i = 0; i < kSpokes; ++i) {
        const unsigned int outer = 1U + i;
        const unsigned int next = 1U + ((i + 1U) % kSpokes);
        indices.push_back(0U);
        indices.push_back(outer);
        indices.push_back(next);
    }

    glm::vec3 expectedTangentAccum(0.0F);
    glm::vec3 expectedBitangentAccum(0.0F);
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const Vertex &v0 = vertices[indices[i + 0]];
        const Vertex &v1 = vertices[indices[i + 1]];
        const Vertex &v2 = vertices[indices[i + 2]];
        const glm::vec3 e1 = v1.position - v0.position;
        const glm::vec3 e2 = v2.position - v0.position;
        const glm::vec2 d1 = v1.texture_coords - v0.texture_coords;
        const glm::vec2 d2 = v2.texture_coords - v0.texture_coords;
        const float det = (d1.x * d2.y) - (d2.x * d1.y);
        if (glm::abs(det) < 1e-8F) { continue; }
        const float invDet = 1.0F / det;
        expectedTangentAccum += ((e1 * d2.y) - (e2 * d1.y)) * invDet;
        expectedBitangentAccum += ((e2 * d1.x) - (e1 * d2.x)) * invDet;
    }
    glm::vec3 expectedTangent = expectedTangentAccum - (normal * glm::dot(normal, expectedTangentAccum));
    ASSERT_GT(glm::dot(expectedTangent, expectedTangent), 1e-12F)
      << "fixture must not hit the degenerate-UV fallback path";
    expectedTangent = glm::normalize(expectedTangent);
    const float expectedW =
      glm::dot(glm::cross(normal, expectedTangent), expectedBitangentAccum) < 0.0F ? -1.0F : 1.0F;

    vertex::computeTangents(vertices, indices);

    const glm::vec4 &actual = vertices[0].tangent;
    EXPECT_NEAR(actual.x, expectedTangent.x, 1e-5F);
    EXPECT_NEAR(actual.y, expectedTangent.y, 1e-5F);
    EXPECT_NEAR(actual.z, expectedTangent.z, 1e-5F);
    EXPECT_EQ(actual.w, expectedW);
}

TEST(VertexUnit, TangentParticipatesInEqualityAndHash)
{
    // operator== and std::hash must both include tangent, as they do color.
    const glm::vec3 pos(1.0F, 2.0F, 3.0F);
    const glm::vec3 normal(0.0F, 1.0F, 0.0F);
    const glm::vec4 color(1.0F);
    const glm::vec2 uv(0.25F, 0.75F);

    const Vertex a(pos, normal, color, uv, glm::vec4(1.0F, 0.0F, 0.0F, 1.0F));
    const Vertex b(pos, normal, color, uv, glm::vec4(0.0F, 1.0F, 0.0F, 1.0F));

    EXPECT_FALSE(a == b);
    EXPECT_NE(std::hash<Vertex>()(a), std::hash<Vertex>()(b));
}
