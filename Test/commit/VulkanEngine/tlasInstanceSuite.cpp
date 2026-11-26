// A TLAS instance's transform is the model matrix's top three rows, and its 24-bit custom index must hold the mesh base.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>

#include "renderer/accelerationStructures/TlasInstance.hpp"

namespace {

auto sampleModelMatrix() -> glm::mat4
{
    const glm::mat4 translated = glm::translate(glm::mat4(1.0F), glm::vec3(3.0F, -2.0F, 5.0F));
    const glm::mat4 rotated = glm::rotate(translated, glm::radians(30.0F), glm::normalize(glm::vec3(1.0F, 2.0F, 3.0F)));
    return glm::scale(rotated, glm::vec3(2.0F, 0.5F, 1.5F));
}

}// namespace

TEST(TlasInstanceUnit, TransformMovesAPointAsTheModelMatrixDoes)
{
    const glm::mat4 model = sampleModelMatrix();
    const vk::TransformMatrixKHR transform = Kataglyphis::tlasInstanceTransform(model);

    const glm::vec4 point(0.25F, -1.5F, 4.0F, 1.0F);
    const glm::vec4 expected = model * point;
    for (std::size_t row = 0; row < 3; ++row) {
        const float traced = (transform.matrix[row][0] * point.x) + (transform.matrix[row][1] * point.y)
                             + (transform.matrix[row][2] * point.z) + transform.matrix[row][3];
        EXPECT_NEAR(traced, expected[static_cast<glm::length_t>(row)], 1e-5F) << "row " << row;
    }
}

TEST(TlasInstanceUnit, TranslationLandsInTheLastColumn)
{
    const vk::TransformMatrixKHR transform =
      Kataglyphis::tlasInstanceTransform(glm::translate(glm::mat4(1.0F), glm::vec3(7.0F, 8.0F, 9.0F)));
    EXPECT_FLOAT_EQ(transform.matrix[0][3], 7.0F);
    EXPECT_FLOAT_EQ(transform.matrix[1][3], 8.0F);
    EXPECT_FLOAT_EQ(transform.matrix[2][3], 9.0F);
    EXPECT_FLOAT_EQ(transform.matrix[0][0], 1.0F);
    EXPECT_FLOAT_EQ(transform.matrix[1][1], 1.0F);
    EXPECT_FLOAT_EQ(transform.matrix[2][2], 1.0F);
}

// The layout the memcpy of glm::transpose(model) gave before: the first twelve floats of the transposed matrix.
TEST(TlasInstanceUnit, TransformMatchesTheTransposedMatrixsFirstThreeColumns)
{
    const glm::mat4 model = sampleModelMatrix();
    const glm::mat4 transposed = glm::transpose(model);
    const vk::TransformMatrixKHR transform = Kataglyphis::tlasInstanceTransform(model);
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            EXPECT_EQ(transform.matrix[row][column],
              transposed[static_cast<glm::length_t>(row)][static_cast<glm::length_t>(column)])
              << "row " << row << ", column " << column;
        }
    }
}

TEST(TlasInstanceUnit, MeshBasesUpToTheTwentyFourBitMaximumFit)
{
    EXPECT_TRUE(Kataglyphis::meshBasesFitInstanceCustomIndex(std::vector<uint32_t>{}));
    EXPECT_TRUE(Kataglyphis::meshBasesFitInstanceCustomIndex(std::vector<uint32_t>{ 0, 12, 40 }));
    EXPECT_TRUE(
      Kataglyphis::meshBasesFitInstanceCustomIndex(std::vector<uint32_t>{ 0, Kataglyphis::kMaxInstanceCustomIndex }));
    EXPECT_EQ(Kataglyphis::kMaxInstanceCustomIndex, 0xFFFFFFU);
}

TEST(TlasInstanceUnit, AMeshBasePastTwentyFourBitsDoesNotFit)
{
    EXPECT_FALSE(Kataglyphis::meshBasesFitInstanceCustomIndex(
      std::vector<uint32_t>{ 0, Kataglyphis::kMaxInstanceCustomIndex + 1U }));
}
