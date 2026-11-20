// Each renderer used to mirror the other's matrices only in comments; Rust's are in WebGPU clip space (Y up).

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "common/GuiModelTransform.hpp"
#include "common/SceneUboMarshal.hpp"
#include "oxidant_bridge/parity.h"

namespace {

namespace px = kataglyphis::oxidant::parity;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

glm::mat4 toGlm(const px::Matrix4 &matrix)
{
    glm::mat4 out(0.0F);
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            out[column][row] = matrix.columns[static_cast<std::size_t>(column * 4 + row)];
        }
    }
    return out;
}

// Vulkan's clip Y points down, WebGPU's up: the one difference between the two renderers' matrices.
const glm::mat4 kFlipY =
  glm::mat4(glm::vec4(1, 0, 0, 0), glm::vec4(0, -1, 0, 0), glm::vec4(0, 0, 1, 0), glm::vec4(0, 0, 0, 1));

// glm and glam round tan and the products differently in the last bits, nothing more.
void expectNear(const glm::mat4 &cpp, const glm::mat4 &rust, const std::string &what)
{
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            const float tolerance = 1e-5F * std::max(1.0F, std::abs(rust[column][row]));
            EXPECT_NEAR(cpp[column][row], rust[column][row], tolerance)
              << what << ": column " << column << ", row " << row;
        }
    }
}

// What the Rust renderer hands its lens: width over height in floats, infinite or NaN for a zero height.
float frameAspect(std::uint32_t width, std::uint32_t height)
{
    if (height > 0) { return static_cast<float>(width) / static_cast<float>(height); }
    return width > 0 ? kInf : kNaN;
}

struct RectCase
{
    std::string name;
    px::ViewportRect rect;
};

std::vector<RectCase> rectCases()
{
    return {
        { "the whole frame", { 0.0F, 0.0F, 1.0F, 1.0F } },
        { "the editor's docked rect", { 0.155F, 0.04F, 0.79F, 0.77F } },
        { "edges past the frame", { -0.5F, 0.2F, 1.5F, 0.9F } },
        { "NaN edges", { kNaN, 0.1F, 0.9F, kNaN } },
        { "a collapsed rect", { 0.5F, 0.5F, 0.52F, 0.9F } },
        { "an inverted rect", { 0.8F, 0.1F, 0.2F, 0.9F } },
    };
}

}// namespace

TEST(RendererParity, ViewportRemapMatchesOxidantsWithItsYTranslationNegated)
{
    for (const RectCase &input : rectCases()) {
        const px::ViewportRemap rust = px::parity_viewport_remap(input.rect);
        const Kataglyphis::ViewportFraction sanitized =
          Kataglyphis::sanitizeViewportFraction(input.rect.x0, input.rect.y0, input.rect.x1, input.rect.y1);
        EXPECT_EQ(sanitized.x0, rust.sanitized.x0) << input.name;
        EXPECT_EQ(sanitized.y0, rust.sanitized.y0) << input.name;
        EXPECT_EQ(sanitized.x1, rust.sanitized.x1) << input.name;
        EXPECT_EQ(sanitized.y1, rust.sanitized.y1) << input.name;
        expectNear(Kataglyphis::makeViewportRemap(sanitized), kFlipY * toGlm(rust.remap) * kFlipY, input.name);
    }
}

TEST(RendererParity, LensMatchesOxidantsWithRowOneNegated)
{
    struct Frame
    {
        std::uint32_t width;
        std::uint32_t height;
    };
    constexpr float kNear = 0.1F;
    for (const float fov : { 20.0F, 45.0F, 110.0F }) {
        // An empty frame (a minimized window) has aspect 1 in both renderers, whichever side is zero.
        for (const Frame frame : { Frame{ 1600, 900 },
               Frame{ 1920, 1080 },
               Frame{ 900, 1600 },
               Frame{ 1600, 0 },
               Frame{ 0, 768 },
               Frame{ 0, 0 } }) {
            for (const float far : { 150.0F, 4000.0F }) {
                for (const RectCase &input : rectCases()) {
                    const Kataglyphis::ViewportFraction rect =
                      Kataglyphis::sanitizeViewportFraction(input.rect.x0, input.rect.y0, input.rect.x1, input.rect.y1);
                    // VulkanRenderer::updateUniforms' projection, step for step.
                    const glm::mat4 cpp =
                      Kataglyphis::makeViewportRemap(rect)
                      * Kataglyphis::makeVulkanProjection(
                        fov, Kataglyphis::viewportAspectRatio(frame.width, frame.height, rect), kNear, far);
                    const px::Lens lens{ .fov_y_deg = fov,
                        .frame_aspect = frameAspect(frame.width, frame.height),
                        .near_plane = kNear,
                        .far_plane = far,
                        .viewport = input.rect };
                    expectNear(cpp,
                      kFlipY * toGlm(px::parity_lens(lens)),
                      input.name + ", fov " + std::to_string(fov) + ", " + std::to_string(frame.width) + "x"
                        + std::to_string(frame.height) + ", far " + std::to_string(far));
                }
            }
        }
    }
}

TEST(RendererParity, ModelTransformMatchesOxidants)
{
    struct Pose
    {
        std::string name;
        std::array<float, 3> position;
        std::array<float, 3> rotation;
    };
    const std::vector<Pose> poses{
        { "the identity", { 0.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 0.0F } },
        { "a moved and turned model", { 1.0F, 2.0F, 3.0F }, { 10.0F, 20.0F, 30.0F } },
        { "quarter and half turns", { -4.0F, 0.5F, 7.0F }, { 90.0F, -45.0F, 180.0F } },
        { "NaN fields", { kNaN, 2.0F, 3.0F }, { 30.0F, kNaN, 0.0F } },
        { "infinite fields", { 1.0F, kInf, 3.0F }, { -kInf, 15.0F, 25.0F } },
    };
    for (const Pose &pose : poses) {
        const glm::mat4 cpp = Kataglyphis::makeGuiModelTransform(
          std::span<const float, 3>(pose.position), std::span<const float, 3>(pose.rotation));
        const px::Matrix4 rust = px::parity_model_transform(px::ModelPose{ pose.position, pose.rotation });
        expectNear(cpp, toGlm(rust), pose.name);
    }
}
