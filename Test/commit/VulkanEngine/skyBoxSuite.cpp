// The upload copies layerSize bytes from every face, so a mismatched face reads or writes out of bounds.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "ScopedEnvVar.hpp"
#include "common/SceneUboMarshal.hpp"
#include "scene/sky_box/AnalyticSky.hpp"
#include "scene/sky_box/CloudAmbient.hpp"
#include "scene/sky_box/CubemapFaces.hpp"
#include "scene/sky_box/SkyUniforms.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace {
constexpr int kConstexprEqualFaces[6] = { 64, 64, 64, 64, 64, 64 };
}// namespace

static_assert(Kataglyphis::cubemapFacesConsistent(kConstexprEqualFaces, kConstexprEqualFaces),
  "cubemapFacesConsistent must be usable in a constant expression");

TEST(SkyBoxUnit, CubemapFacesConsistentAcceptsSixEqualFaces)
{
    const int widths[6] = { 64, 64, 64, 64, 64, 64 };
    const int heights[6] = { 64, 64, 64, 64, 64, 64 };
    EXPECT_TRUE(Kataglyphis::cubemapFacesConsistent(widths, heights));
}

TEST(SkyBoxUnit, RejectsAMismatchedFace)
{
    const int widths[6] = { 64, 64, 32, 64, 64, 64 };
    const int heights[6] = { 64, 64, 64, 64, 64, 64 };
    EXPECT_FALSE(Kataglyphis::cubemapFacesConsistent(widths, heights));
}

TEST(SkyBoxUnit, RejectsADegenerateFace)
{
    const int widths[6] = { 64, 64, 64, 64, 64, 0 };
    const int heights[6] = { 64, 64, 64, 64, 64, 64 };
    EXPECT_FALSE(Kataglyphis::cubemapFacesConsistent(widths, heights));
}

TEST(SkyBoxUnit, RejectsADegenerateFirstFace)
{
    const int widths[6] = { 0, 64, 64, 64, 64, 64 };
    const int heights[6] = { 64, 64, 64, 64, 64, 64 };
    EXPECT_FALSE(Kataglyphis::cubemapFacesConsistent(widths, heights));
}

TEST(SkyBoxUnit, RejectsSixEqualNonSquareFaces)
{
    // A cube-compatible image must be square (VUID-VkImageCreateInfo-imageType-00954); Rust refuses these too.
    const int widths[6] = { 128, 128, 128, 128, 128, 128 };
    const int heights[6] = { 64, 64, 64, 64, 64, 64 };
    EXPECT_FALSE(Kataglyphis::cubemapFacesConsistent(widths, heights));
}

TEST(SkyBoxUnit, AcceptsAllLargeEqualFaces)
{
    const int widths[6] = { 128, 128, 128, 128, 128, 128 };
    const int heights[6] = { 128, 128, 128, 128, 128, 128 };
    EXPECT_TRUE(Kataglyphis::cubemapFacesConsistent(widths, heights));
}

TEST(SkyBoxUnit, TheSkyboxDirectoryOverrideIsReadAtUse)
{
    {
        const Kataglyphis::TestSupport::ScopedEnvVar unset("KATAGLYPHIS_SKYBOX_DIR", "");
        EXPECT_FALSE(Kataglyphis::skyboxDirectoryOverride().has_value());
    }
    const Kataglyphis::TestSupport::ScopedEnvVar elsewhere("KATAGLYPHIS_SKYBOX_DIR", "some/other/sky");
    const auto overridden = Kataglyphis::skyboxDirectoryOverride();
    ASSERT_TRUE(overridden.has_value());
    EXPECT_EQ(*overridden, std::filesystem::path("some/other/sky"));
}

namespace {

// Six RGBA8 faces of one grey; a test then edits texels through face().
struct SyntheticCubemap
{
    uint32_t width;
    uint32_t height;
    std::array<std::vector<unsigned char>, 6> storage;

    SyntheticCubemap(uint32_t w, uint32_t h, unsigned char grey) : width(w), height(h)
    {
        for (auto &face : storage) {
            face.assign(static_cast<size_t>(w) * h * 4U, grey);
            for (size_t texel = 0; texel < static_cast<size_t>(w) * h; ++texel) { face[texel * 4U + 3U] = 255; }
        }
    }

    void set(size_t face, uint32_t row, uint32_t column, unsigned char grey)
    {
        unsigned char *texel = storage[face].data() + (static_cast<size_t>(row) * width + column) * 4U;
        texel[0] = texel[1] = texel[2] = grey;
    }

    Kataglyphis::CloudAmbient ambient() const
    {
        const std::array<const unsigned char *, 6> faces{ storage[0].data(),
            storage[1].data(),
            storage[2].data(),
            storage[3].data(),
            storage[4].data(),
            storage[5].data() };
        return Kataglyphis::cloudAmbientOfCubemap(faces, width, height);
    }
};

float radianceOf(unsigned char grey) { return Kataglyphis::acesTonemapInverse(Kataglyphis::srgbChannelToLinear(grey)); }

}// namespace

TEST(SkyBoxUnit, AUniformCubemapLightsBothHemispheresWithItsUntonemappedRadiance)
{
    const auto ambient = SyntheticCubemap(4, 4, 128).ambient();
    const float expected = radianceOf(128);
    // sky.slang untonemaps the photo sky, so the radiance is what ACES maps back onto the decoded texel.
    const float aces = (expected * (2.51F * expected + 0.03F)) / (expected * (2.43F * expected + 0.59F) + 0.14F);
    EXPECT_NEAR(aces, Kataglyphis::srgbChannelToLinear(128), 1e-5F);
    EXPECT_GT(std::abs(expected - Kataglyphis::srgbChannelToLinear(128)), 0.01F);
    for (int c = 0; c < 3; ++c) {
        EXPECT_FLOAT_EQ(ambient.top[c], expected);
        EXPECT_FLOAT_EQ(ambient.bottom[c], expected);
    }
}

TEST(SkyBoxUnit, TheCeilingAndTheSideFacesUpperRowsLightTheTop)
{
    // +Y bright, -Y dark, every side face bright above its middle row and dark below it.
    SyntheticCubemap cubemap(4, 4, 0);
    for (size_t face = 0; face < 6; ++face) {
        for (uint32_t row = 0; row < 4; ++row) {
            const bool up = face == 2 || (face != 3 && row < 2);
            for (uint32_t column = 0; column < 4; ++column) { cubemap.set(face, row, column, up ? 200 : 20); }
        }
    }
    const auto ambient = cubemap.ambient();
    EXPECT_FLOAT_EQ(ambient.top.x, radianceOf(200));
    EXPECT_FLOAT_EQ(ambient.bottom.x, radianceOf(20));
}

TEST(SkyBoxUnit, ASideFacesMiddleRowCountsTowardBothHemispheres)
{
    // Three rows: the middle one looks at the horizon; only it is bright, on the side faces.
    SyntheticCubemap cubemap(3, 3, 0);
    for (size_t face : { 0U, 1U, 4U, 5U }) {
        for (uint32_t column = 0; column < 3; ++column) { cubemap.set(face, 1, column, 255); }
    }
    const auto ambient = cubemap.ambient();
    // Per hemisphere: 9 ceiling or floor texels, 12 side texels off the middle row, 12 half-weighted middle ones.
    const float expected = 4.0F * 3.0F * 0.5F * radianceOf(255) / (9.0F + 12.0F + 6.0F);
    EXPECT_NEAR(ambient.top.x, expected, 1e-5F);
    EXPECT_NEAR(ambient.bottom.x, expected, 1e-5F);
}

TEST(SkyBoxUnit, ALargeCubemapIsAveragedOverAtMost64TexelsPerEdge)
{
    // Only every fourth texel of a 256-texel edge is read, so a value off that grid never shows.
    SyntheticCubemap cubemap(256, 256, 100);
    for (size_t face = 0; face < 6; ++face) {
        for (uint32_t row = 0; row < 256; ++row) {
            for (uint32_t column = 0; column < 256; ++column) {
                if (row % 4U != 0U || column % 4U != 0U) { cubemap.set(face, row, column, 250); }
            }
        }
    }
    static_assert(256 / Kataglyphis::kCloudAmbientMaxEdgeTexels == 4);
    const auto ambient = cubemap.ambient();
    EXPECT_FLOAT_EQ(ambient.top.x, radianceOf(100));
    EXPECT_FLOAT_EQ(ambient.bottom.x, radianceOf(100));
}

TEST(SkyBoxUnit, FacesPastTheCubeLimitAreHalvedUntilTheyFit)
{
    // maxImageDimensionCube bounds a face; a larger one was invalid usage that aborted the engine.
    SyntheticCubemap cubemap(8, 8, 100);
    cubemap.set(2, 0, 0, 255);
    const std::array<const unsigned char *, 6> faces{ cubemap.storage[0].data(),
        cubemap.storage[1].data(),
        cubemap.storage[2].data(),
        cubemap.storage[3].data(),
        cubemap.storage[4].data(),
        cubemap.storage[5].data() };

    const Kataglyphis::FittedCubemap fitted = Kataglyphis::fitCubemapFacesToLimit(faces, 8, 4);
    EXPECT_EQ(fitted.size, 4U);
    EXPECT_EQ(fitted.halvings, 1U);
    ASSERT_EQ(fitted.texels.size(), 6U * 4U * 4U * 4U);
    // +Y's first texel averages the bright one with three greys in linear space, as the sRGB format samples them.
    const unsigned char *plus_y = fitted.texels.data() + 2U * 4U * 4U * 4U;
    const float linear = (Kataglyphis::srgbChannelToLinear(255) + 3.0F * Kataglyphis::srgbChannelToLinear(100)) / 4.0F;
    EXPECT_EQ(plus_y[0], Kataglyphis::linearToSrgbByte(linear));
    EXPECT_EQ(fitted.texels[0], 100) << "the other faces stay their grey";

    const Kataglyphis::FittedCubemap kept = Kataglyphis::fitCubemapFacesToLimit(faces, 8, 8);
    EXPECT_EQ(kept.size, 8U);
    EXPECT_EQ(kept.halvings, 0U);
}

TEST(SkyBoxUnit, TheDisabledSkyLightsTheCloudsWithItsUntonemappedNavy)
{
    const auto ambient = Kataglyphis::cloudAmbientOfDisabledSky();
    EXPECT_FLOAT_EQ(ambient.top.x, 0.0F);
    EXPECT_FLOAT_EQ(ambient.top.y, 0.0F);
    EXPECT_FLOAT_EQ(ambient.top.z, Kataglyphis::acesTonemapInverse(0.5F));
    EXPECT_EQ(ambient.top, ambient.bottom);
    // ACES maps this radiance back to the navy's 0.5: (x(ax + b)) / (x(cx + d) + e).
    const float x = ambient.top.z;
    EXPECT_NEAR((x * (2.51F * x + 0.03F)) / (x * (2.43F * x + 0.59F) + 0.14F), 0.5F, 1e-5F);
}

namespace {

// The texel of a baked face, decoded back to the display value the drawn sky shows.
glm::vec3 analyticTexel(const std::vector<unsigned char> &faces, size_t face, uint32_t column, uint32_t row)
{
    const uint32_t size = Kataglyphis::kAnalyticSkyFaceSize;
    const unsigned char *texel = faces.data() + ((face * size + row) * size + column) * 4U;
    return { Kataglyphis::srgbChannelToLinear(texel[0]),
        Kataglyphis::srgbChannelToLinear(texel[1]),
        Kataglyphis::srgbChannelToLinear(texel[2]) };
}

glm::vec3 tonemapped(glm::vec3 radiance)
{
    return {
        Kataglyphis::acesTonemap(radiance.x), Kataglyphis::acesTonemap(radiance.y), Kataglyphis::acesTonemap(radiance.z)
    };
}

}// namespace

// The missing-skybox sky is OxidANT's gradient: zenith above, horizon at a side face's middle, ground below.
TEST(SkyBoxUnit, TheAnalyticSkyBakesZenithHorizonAndGroundWhereTheyBelong)
{
    const uint32_t size = Kataglyphis::kAnalyticSkyFaceSize;
    const auto faces = Kataglyphis::analyticSkyFaces(size);
    ASSERT_EQ(faces.size(), static_cast<size_t>(size) * size * 4U * 6U);
    const auto near = [](glm::vec3 got, glm::vec3 want, float tolerance) {
        return std::abs(got.x - want.x) < tolerance && std::abs(got.y - want.y) < tolerance
               && std::abs(got.z - want.z) < tolerance;
    };
    // +Y's centre looks straight up, -Y's straight down; a byte's rounding is under 0.01 in linear.
    EXPECT_TRUE(near(analyticTexel(faces, 2, size / 2, size / 2), tonemapped(Kataglyphis::kAnalyticSkyZenith), 0.01F));
    EXPECT_TRUE(near(analyticTexel(faces, 3, size / 2, size / 2), tonemapped(Kataglyphis::kAnalyticSkyGround), 0.01F));
    // A side face's middle rows straddle the horizon, its top row looks up.
    for (size_t face : { 0U, 1U, 4U, 5U }) {
        const glm::vec3 horizon = analyticTexel(faces, face, size / 2, size / 2);
        EXPECT_TRUE(near(horizon, tonemapped(Kataglyphis::kAnalyticSkyHorizon), 0.03F)) << "face " << face;
        EXPECT_LT(analyticTexel(faces, face, size / 2, 0).x, horizon.x) << "face " << face << ": the top row is zenith";
        EXPECT_GT(analyticTexel(faces, face, size / 2, 0).z, analyticTexel(faces, face, size / 2, 0).x);
    }
    for (size_t at = 3; at < faces.size(); at += 4) { ASSERT_EQ(faces[at], 255); }
}

// Every face texel is lit: the black fallback it replaced gave the clouds no ambient at all.
TEST(SkyBoxUnit, TheAnalyticSkyGivesTheCloudsABlueTopAndADarkerBottom)
{
    const uint32_t size = Kataglyphis::kAnalyticSkyFaceSize;
    const auto texels = Kataglyphis::analyticSkyFaces(size);
    const size_t layer = static_cast<size_t>(size) * size * 4U;
    const std::array<const unsigned char *, 6> faces{ texels.data(),
        texels.data() + layer,
        texels.data() + 2 * layer,
        texels.data() + 3 * layer,
        texels.data() + 4 * layer,
        texels.data() + 5 * layer };
    const auto ambient = Kataglyphis::cloudAmbientOfCubemap(faces, size, size);
    EXPECT_GT(ambient.top.z, ambient.top.x) << "the upper hemisphere is the blue zenith";
    EXPECT_GT(ambient.top.z, 0.2F);
    EXPECT_GT(ambient.bottom.x, 0.1F) << "the lower hemisphere is horizon and ground, not black";
    EXPECT_LT(ambient.bottom.z, ambient.top.z);
}

namespace {

// sky.slang's fs_main: the world direction from the near to the far point through a clip-space pixel.
glm::vec3 skyDirection(const Kataglyphis::SkyUniforms &uniforms, glm::vec2 ndc)
{
    const glm::vec4 near_point = uniforms.inv_view_proj * glm::vec4(ndc, 0.0F, 1.0F);
    const glm::vec4 far_point = uniforms.inv_view_proj * glm::vec4(ndc, 1.0F, 1.0F);
    return glm::normalize(glm::vec3(far_point) / far_point.w - glm::vec3(near_point) / near_point.w);
}

}// namespace

// sky.slang draws the cube map above mode 0.5 and the navy above 1.5; C++ never asks for its procedural mode 0.
TEST(SkyBoxUnit, SkyUniformsAskForTheCubeMapOrTheNavy)
{
    const glm::mat4 identity(1.0F);
    const auto shown = Kataglyphis::makeSkyUniforms(identity, identity, glm::vec3(0.0F, -1.0F, 0.0F), 10.0F, true);
    const auto hidden = Kataglyphis::makeSkyUniforms(identity, identity, glm::vec3(0.0F, -1.0F, 0.0F), 10.0F, false);
    EXPECT_GT(shown.mode.x, 0.5F);
    EXPECT_LE(shown.mode.x, 1.5F);
    EXPECT_GT(hidden.mode.x, 1.5F);
}

// The Vulkan projection flips y, so a mismatched inverse would draw the sky upside down or behind the camera.
TEST(SkyBoxUnit, SkyUniformsAimEachPixelAlongItsViewRay)
{
    const glm::vec3 eye(1.0F, 6.0F, 26.0F);
    const glm::vec3 forward = glm::normalize(glm::vec3(0.3F, -0.17F, -1.0F));
    const glm::mat4 view = glm::lookAt(eye, eye + forward, glm::vec3(0.0F, 1.0F, 0.0F));
    const glm::mat4 projection = Kataglyphis::makeVulkanProjection(45.0F, 1200.0F / 768.0F, 0.1F, 150.0F);
    const auto uniforms = Kataglyphis::makeSkyUniforms(projection, view, glm::vec3(0.0F, -1.0F, 0.0F), 10.0F, true);

    const glm::vec3 centre = skyDirection(uniforms, glm::vec2(0.0F, 0.0F));
    EXPECT_NEAR(glm::dot(centre, forward), 1.0F, 1e-4F);
    // Clip y = -1 is the top row in Vulkan: it must look above the centre, as a camera's top row does.
    EXPECT_GT(skyDirection(uniforms, glm::vec2(0.0F, -1.0F)).y, centre.y + 0.1F);
    // Every pixel's ray stays in front of the camera, half the vertical field of view at most from its axis.
    EXPECT_NEAR(
      glm::degrees(std::acos(glm::dot(skyDirection(uniforms, glm::vec2(0.0F, 1.0F)), forward))), 22.5F, 0.01F);
}

TEST(SkyBoxUnit, SkyUniformsPointTheSunTowardsTheLight)
{
    const glm::mat4 identity(1.0F);
    const glm::vec3 travel(-0.55F, -1.0F, -0.35F);
    const auto uniforms = Kataglyphis::makeSkyUniforms(identity, identity, travel, 7.5F, true);
    EXPECT_EQ(glm::vec3(uniforms.light_dir_intensity), -travel);
    EXPECT_FLOAT_EQ(uniforms.light_dir_intensity.w, 7.5F);
}
