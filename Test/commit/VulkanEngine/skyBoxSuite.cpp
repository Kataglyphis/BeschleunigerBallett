// The upload copies layerSize bytes from every face, so a mismatched face reads or writes out of bounds.

#include <gtest/gtest.h>

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
