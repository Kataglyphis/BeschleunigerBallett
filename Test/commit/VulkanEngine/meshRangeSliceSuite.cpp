// Otherwise only reached through GPU rendering; the traps are vertexBase re-basing and half-open bounds.

#include <gtest/gtest.h>

#include <vector>

#include <glm/glm.hpp>

import kataglyphis.vulkan.mesh_range;
import kataglyphis.vulkan.vertex;

using Kataglyphis::MeshRange;
using Kataglyphis::MeshSlice;
using Kataglyphis::sliceMeshRange;
// Vertex lives in the global namespace (Src/shared/scene/Vertex.hpp), not Kataglyphis.
using ::Vertex;

namespace {
Vertex vertexAtX(float x)
{ return Vertex(glm::vec3(x, 0.0F, 0.0F), glm::vec3(0.0F), glm::vec4(1.0F), glm::vec2(0.0F)); }
}// namespace

TEST(MeshRangeSlice, RebasesIndicesAndSlicesArraysForASubMesh)
{
    // Six vertices identified by position.x = 0..5.
    std::vector<Vertex> vertices;
    for (int i = 0; i < 6; ++i) { vertices.push_back(vertexAtX(static_cast<float>(i))); }

    // The last two triangles reference the second sub-mesh's vertices.
    const std::vector<unsigned int> indices = { 0, 1, 2, 3, 4, 5, 3, 5, 4 };
    // One material id per triangle.
    const std::vector<unsigned int> materialIndex = { 0, 1, 1 };

    // Second sub-mesh: vertices [3,6), indices [3,9) (six of them), tris [1,3).
    const MeshRange range{ 3, 3, 3, 6, 1, 2 };

    const MeshSlice slice = sliceMeshRange(range, vertices, indices, materialIndex);

    ASSERT_EQ(slice.vertices.size(), 3U);
    EXPECT_FLOAT_EQ(slice.vertices[0].get_position().x, 3.0F);
    EXPECT_FLOAT_EQ(slice.vertices[1].get_position().x, 4.0F);
    EXPECT_FLOAT_EQ(slice.vertices[2].get_position().x, 5.0F);

    // Each index is re-based by vertexBase (3): global 3,4,5,3,5,4 -> 0,1,2,0,2,1.
    const std::vector<unsigned int> expectedIndices = { 0, 1, 2, 0, 2, 1 };
    EXPECT_EQ(slice.indices, expectedIndices);

    // materialIndex[triStart, triStart + triCount) = [1,3) = {1,1}.
    const std::vector<unsigned int> expectedMaterials = { 1, 1 };
    EXPECT_EQ(slice.materialIndex, expectedMaterials);
}

TEST(MeshRangeSlice, SingleRangeSpanningEverythingReproducesTheInputs)
{
    // vertexBase 0 must behave exactly like not splitting at all.
    std::vector<Vertex> vertices;
    for (int i = 0; i < 3; ++i) { vertices.push_back(vertexAtX(static_cast<float>(i))); }
    const std::vector<unsigned int> indices = { 0, 1, 2 };
    const std::vector<unsigned int> materialIndex = { 0 };
    const MeshRange range{ 0, 3, 0, 3, 0, 1 };

    const MeshSlice slice = sliceMeshRange(range, vertices, indices, materialIndex);

    ASSERT_EQ(slice.vertices.size(), 3U);
    EXPECT_EQ(slice.indices, indices);
    EXPECT_EQ(slice.materialIndex, materialIndex);
}

TEST(MeshRangeSlice, OutOfRangeRangeYieldsAnEmptySliceInsteadOfReadingPastTheArrays)
{
    // Each case breaks one range check; the slice must come back empty, never read past the arrays.
    std::vector<Vertex> vertices;
    for (int i = 0; i < 3; ++i) { vertices.push_back(vertexAtX(static_cast<float>(i))); }
    const std::vector<unsigned int> indices = { 0, 1, 2 };
    const std::vector<unsigned int> materialIndex = { 0 };

    // vertexBase + vertexCount > vertices.size() (3 + 1 > 3).
    {
        const MeshRange range{ 3, 1, 0, 3, 0, 1 };
        const MeshSlice slice = sliceMeshRange(range, vertices, indices, materialIndex);
        EXPECT_TRUE(slice.vertices.empty());
        EXPECT_TRUE(slice.indices.empty());
        EXPECT_TRUE(slice.materialIndex.empty());
    }

    // indexStart + indexCount > indices.size() (1 + 3 > 3).
    {
        const MeshRange range{ 0, 3, 1, 3, 0, 1 };
        const MeshSlice slice = sliceMeshRange(range, vertices, indices, materialIndex);
        EXPECT_TRUE(slice.vertices.empty());
        EXPECT_TRUE(slice.indices.empty());
        EXPECT_TRUE(slice.materialIndex.empty());
    }

    // triStart + triCount > materialIndex.size() (0 + 2 > 1).
    {
        const MeshRange range{ 0, 3, 0, 3, 0, 2 };
        const MeshSlice slice = sliceMeshRange(range, vertices, indices, materialIndex);
        EXPECT_TRUE(slice.vertices.empty());
        EXPECT_TRUE(slice.indices.empty());
        EXPECT_TRUE(slice.materialIndex.empty());
    }
}
