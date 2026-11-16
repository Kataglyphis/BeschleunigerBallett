// The upload copies layerSize bytes from every face, so a mismatched face reads or writes out of bounds.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "scene/sky_box/CloudAmbient.hpp"
#include "scene/sky_box/CubemapFaces.hpp"

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

TEST(SkyBoxUnit, AcceptsAllLargeEqualFaces)
{
    const int widths[6] = { 128, 128, 128, 128, 128, 128 };
    const int heights[6] = { 128, 128, 128, 128, 128, 128 };
    EXPECT_TRUE(Kataglyphis::cubemapFacesConsistent(widths, heights));
}

TEST(SkyBoxUnit, FallbackCubemapFacePixelIsOpaqueBlack)
{
    EXPECT_EQ(Kataglyphis::kFallbackCubemapFacePixel[0], 0);
    EXPECT_EQ(Kataglyphis::kFallbackCubemapFacePixel[1], 0);
    EXPECT_EQ(Kataglyphis::kFallbackCubemapFacePixel[2], 0);
    EXPECT_EQ(Kataglyphis::kFallbackCubemapFacePixel[3], 255);
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
    // The sky pass is untonemapped, so the radiance is what ACES maps back onto the decoded texel.
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

TEST(SkyBoxUnit, TheBlackFallbackCubemapGivesNoAmbient)
{
    const std::array<const unsigned char *, 6> faces{ Kataglyphis::kFallbackCubemapFacePixel,
        Kataglyphis::kFallbackCubemapFacePixel,
        Kataglyphis::kFallbackCubemapFacePixel,
        Kataglyphis::kFallbackCubemapFacePixel,
        Kataglyphis::kFallbackCubemapFacePixel,
        Kataglyphis::kFallbackCubemapFacePixel };
    const auto ambient = Kataglyphis::cloudAmbientOfCubemap(faces, 1, 1);
    EXPECT_EQ(ambient.top, glm::vec3(0.0F));
    EXPECT_EQ(ambient.bottom, glm::vec3(0.0F));
}
