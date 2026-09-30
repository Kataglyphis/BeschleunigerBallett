// The per-mesh accessors must bounds-check the mesh index too, not only the model index.

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <vulkan/vulkan.hpp>

#include <glm/gtc/matrix_transform.hpp>

import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.model;

using Kataglyphis::Model;
using Kataglyphis::Scene;

TEST(SceneAccessorUnit, OutOfRangeMeshIndexReturnsTheSafeFallback)
{
    Scene scene;
    scene.add_model(std::make_shared<Model>());

    EXPECT_EQ(scene.getMeshCount(0), 0U);
    EXPECT_EQ(scene.getIndexCount(0, 0), 0U);
    EXPECT_FALSE(scene.isMeshDoubleSided(0, 0));
    EXPECT_EQ(scene.getVertexBuffer(0, 0), vk::Buffer{});
    EXPECT_EQ(scene.getIndexBuffer(0, 0), vk::Buffer{});

    const auto &bounds = scene.getMeshBounds(0, 0);
    EXPECT_GT(bounds.min.x, bounds.max.x) << "an out-of-range mesh must return the inverted 'unknown' box";
}

TEST(SceneAccessorUnit, AddModelIgnoresNull)
{
    Scene scene;
    scene.add_model(nullptr);
    EXPECT_EQ(scene.getModelCount(), 0U) << "a null model (a failed load upstream) must leave the scene unchanged";
}

TEST(SceneAccessorUnit, TheConfigTransformLandsOnTheModelThatWasAdded)
{
    Scene scene;
    scene.add_model(std::make_shared<Model>());
    const glm::mat4 model_zero_matrix_before_second_add = scene.getModelMatrix(0);
    const std::optional<uint32_t> index = scene.add_model(std::make_shared<Model>());

    ASSERT_TRUE(index.has_value());
    EXPECT_EQ(*index, 1U);

    const glm::mat4 transform = glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    scene.update_model_matrix(transform, *index);

    EXPECT_EQ(scene.getModelMatrix(1), transform);
    EXPECT_EQ(scene.getModelMatrix(0), model_zero_matrix_before_second_add)
      << "the transform for the model just added must not land on model 0";
}

TEST(SceneAccessorUnit, AddingANullModelReturnsNoIndex)
{
    Scene scene;
    EXPECT_EQ(scene.add_model(nullptr), std::nullopt);
    EXPECT_EQ(scene.getModelCount(), 0U);
}

TEST(SceneAccessorUnit, PerModelCountVectorsAreSizedByModelCount)
{
    Scene scene;

    EXPECT_EQ(scene.getTextureCountPerModel().size(), 0U);
    EXPECT_EQ(scene.getMeshCountPerModel().size(), 0U);

    scene.add_model(std::make_shared<Model>());
    scene.add_model(std::make_shared<Model>());

    const std::vector<uint32_t> texture_counts = scene.getTextureCountPerModel();
    const std::vector<uint32_t> mesh_counts = scene.getMeshCountPerModel();

    ASSERT_EQ(texture_counts.size(), scene.getModelCount());
    ASSERT_EQ(mesh_counts.size(), scene.getModelCount());

    for (uint32_t i = 0; i < scene.getModelCount(); ++i) {
        EXPECT_EQ(texture_counts[i], scene.getTextureCount(i))
          << "the vector form and the indexed form must never drift, model " << i;
        EXPECT_EQ(mesh_counts[i], scene.getMeshCount(i))
          << "the vector form and the indexed form must never drift, model " << i;
    }
}

// All accessors share findModel()/findMesh(), so each documented fallback is pinned here.
TEST(SceneAccessorUnit, EveryAccessorReturnsItsDocumentedFallbackOutOfRange)
{
    Scene scene;
    scene.add_model(std::make_shared<Model>());
    const uint32_t out_of_range = scene.getModelCount();

    EXPECT_TRUE(scene.getTextures(out_of_range).empty());
    EXPECT_TRUE(scene.getTextureSampler(out_of_range).empty());
    EXPECT_EQ(scene.getTextureCount(out_of_range), 0U);
    EXPECT_EQ(scene.getModelMatrix(out_of_range), glm::mat4(1.0F));
    EXPECT_EQ(scene.getMeshCount(out_of_range), 0U);
    EXPECT_EQ(scene.getVertexBuffer(out_of_range, 0), vk::Buffer{});
    EXPECT_EQ(scene.getIndexBuffer(out_of_range, 0), vk::Buffer{});
    EXPECT_EQ(scene.getIndexCount(out_of_range, 0), 0U);
    EXPECT_FALSE(scene.isMeshDoubleSided(out_of_range, 0));

    const auto &bounds = scene.getMeshBounds(out_of_range, 0);
    EXPECT_GT(bounds.min.x, bounds.max.x) << "an out-of-range model must return the inverted 'unknown' box";
}

TEST(SceneAccessorUnit, UpdateModelMatrixOutOfRangeLeavesEveryModelUntouched)
{
    Scene scene;
    scene.add_model(std::make_shared<Model>());
    const glm::mat4 model_zero_matrix_before = scene.getModelMatrix(0);

    scene.update_model_matrix(glm::scale(glm::mat4(1.0F), glm::vec3(2.0F)), scene.getModelCount());

    EXPECT_EQ(scene.getModelMatrix(0), model_zero_matrix_before)
      << "an out-of-range model_id must not touch any existing model";
}

TEST(SceneAccessorUnit, FindMeshAgreesWithThePerMeshAccessors)
{
    Scene scene;
    scene.add_model(std::make_shared<Model>());

    EXPECT_EQ(scene.findMesh(0, 0), nullptr) << "the added model has zero meshes";
    EXPECT_NE(scene.findModel(0), nullptr);
    EXPECT_EQ(scene.findModel(1), nullptr);
}
