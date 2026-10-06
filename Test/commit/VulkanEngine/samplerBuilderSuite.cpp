#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vulkan/vulkan.hpp>

import kataglyphis.vulkan.sampler_builder;

using Kataglyphis::buildSamplerCreateInfo;
using Kataglyphis::findSampler;
using Kataglyphis::GltfSamplerDesc;
using Kataglyphis::SamplerKey;
using Kataglyphis::usesNearestFiltering;

TEST(SamplerBuilderUnit, MatchesPostStageOffscreenSamplerConfiguration)
{
    const vk::SamplerCreateInfo info = buildSamplerCreateInfo(
      vk::Filter::eLinear, vk::SamplerAddressMode::eRepeat, 0.0F, VK_TRUE, 16.0F, vk::BorderColor::eFloatOpaqueBlack);

    EXPECT_EQ(info.magFilter, vk::Filter::eLinear);
    EXPECT_EQ(info.minFilter, vk::Filter::eLinear);
    EXPECT_EQ(info.addressModeU, vk::SamplerAddressMode::eRepeat);
    EXPECT_EQ(info.addressModeV, vk::SamplerAddressMode::eRepeat);
    EXPECT_EQ(info.addressModeW, vk::SamplerAddressMode::eRepeat);
    EXPECT_EQ(info.borderColor, vk::BorderColor::eFloatOpaqueBlack);
    EXPECT_EQ(info.unnormalizedCoordinates, static_cast<vk::Bool32>(VK_FALSE));
    EXPECT_EQ(info.mipmapMode, vk::SamplerMipmapMode::eLinear);
    EXPECT_FLOAT_EQ(info.mipLodBias, 0.0F);
    EXPECT_FLOAT_EQ(info.minLod, 0.0F);
    EXPECT_FLOAT_EQ(info.maxLod, 0.0F);
    EXPECT_EQ(info.anisotropyEnable, static_cast<vk::Bool32>(VK_TRUE));
    EXPECT_FLOAT_EQ(info.maxAnisotropy, 16.0F);
}

TEST(SamplerBuilderUnit, MatchesModelSamplerConfigurationWithMipLevelAsMaxLod)
{
    const vk::SamplerCreateInfo info = buildSamplerCreateInfo(
      vk::Filter::eLinear, vk::SamplerAddressMode::eRepeat, 4.0F, VK_FALSE, 1.0F, vk::BorderColor::eFloatOpaqueBlack);

    EXPECT_EQ(info.magFilter, vk::Filter::eLinear);
    EXPECT_EQ(info.minFilter, vk::Filter::eLinear);
    EXPECT_EQ(info.addressModeU, vk::SamplerAddressMode::eRepeat);
    EXPECT_EQ(info.borderColor, vk::BorderColor::eFloatOpaqueBlack);
    // maxLod is the one field the call sites genuinely disagree on.
    EXPECT_FLOAT_EQ(info.maxLod, 4.0F);
    EXPECT_EQ(info.anisotropyEnable, static_cast<vk::Bool32>(VK_FALSE));
    EXPECT_FLOAT_EQ(info.maxAnisotropy, 1.0F);
}

TEST(SamplerBuilderUnit, MatchesTextureSamplerConfigurationWithAnisotropyHardDisabled)
{
    const vk::SamplerCreateInfo info = buildSamplerCreateInfo(vk::Filter::eNearest,
      vk::SamplerAddressMode::eClampToEdge,
      5.0F,
      VK_FALSE,
      1.0F,
      vk::BorderColor::eIntOpaqueBlack);

    EXPECT_EQ(info.magFilter, vk::Filter::eNearest);
    EXPECT_EQ(info.minFilter, vk::Filter::eNearest);
    EXPECT_EQ(info.addressModeU, vk::SamplerAddressMode::eClampToEdge);
    EXPECT_EQ(info.addressModeV, vk::SamplerAddressMode::eClampToEdge);
    EXPECT_EQ(info.addressModeW, vk::SamplerAddressMode::eClampToEdge);
    // Texture::createTextureSampler disables anisotropy regardless of device support; a known asymmetry.
    EXPECT_EQ(info.anisotropyEnable, static_cast<vk::Bool32>(VK_FALSE));
    EXPECT_FLOAT_EQ(info.maxAnisotropy, 1.0F);
    EXPECT_EQ(info.borderColor, vk::BorderColor::eIntOpaqueBlack);
    EXPECT_FLOAT_EQ(info.maxLod, 5.0F);
    // Omitted compare arguments default to VK_FALSE/eNever.
    EXPECT_EQ(info.compareEnable, static_cast<vk::Bool32>(VK_FALSE));
    EXPECT_EQ(info.compareOp, vk::CompareOp::eNever);
}

TEST(SamplerBuilderUnit, MatchesShadowMapComparisonSamplerConfiguration)
{
    // Linear filtering plus a compare op makes SampleCmpLevelZero a real bilinear PCF tap.
    const vk::SamplerCreateInfo info = buildSamplerCreateInfo(vk::Filter::eLinear,
      vk::SamplerAddressMode::eClampToEdge,
      0.0F,
      VK_FALSE,
      1.0F,
      vk::BorderColor::eIntOpaqueBlack,
      VK_TRUE,
      vk::CompareOp::eLessOrEqual);

    EXPECT_EQ(info.compareEnable, static_cast<vk::Bool32>(VK_TRUE));
    EXPECT_EQ(info.compareOp, vk::CompareOp::eLessOrEqual);
}

TEST(SamplerBuilderUnit, ScalarOverloadDelegatesToTheDescOverload)
{
    const auto expectSameFields = [](const vk::SamplerCreateInfo &scalar, const vk::SamplerCreateInfo &desc) {
        EXPECT_EQ(scalar.magFilter, desc.magFilter);
        EXPECT_EQ(scalar.minFilter, desc.minFilter);
        EXPECT_EQ(scalar.addressModeU, desc.addressModeU);
        EXPECT_EQ(scalar.addressModeV, desc.addressModeV);
        EXPECT_EQ(scalar.addressModeW, desc.addressModeW);
        EXPECT_EQ(scalar.borderColor, desc.borderColor);
        EXPECT_EQ(scalar.unnormalizedCoordinates, desc.unnormalizedCoordinates);
        EXPECT_EQ(scalar.mipmapMode, desc.mipmapMode);
        EXPECT_FLOAT_EQ(scalar.mipLodBias, desc.mipLodBias);
        EXPECT_FLOAT_EQ(scalar.minLod, desc.minLod);
        EXPECT_FLOAT_EQ(scalar.maxLod, desc.maxLod);
        EXPECT_EQ(scalar.anisotropyEnable, desc.anisotropyEnable);
        EXPECT_FLOAT_EQ(scalar.maxAnisotropy, desc.maxAnisotropy);
        EXPECT_EQ(scalar.compareEnable, desc.compareEnable);
        EXPECT_EQ(scalar.compareOp, desc.compareOp);
    };

    {
        // Default-ish parameter set.
        const vk::SamplerCreateInfo scalar = buildSamplerCreateInfo(vk::Filter::eLinear,
          vk::SamplerAddressMode::eRepeat,
          4.0F,
          VK_TRUE,
          16.0F,
          vk::BorderColor::eFloatOpaqueBlack);
        const vk::SamplerCreateInfo desc =
          buildSamplerCreateInfo(GltfSamplerDesc{ .addressModeU = vk::SamplerAddressMode::eRepeat,
                                   .addressModeV = vk::SamplerAddressMode::eRepeat,
                                   .magFilter = vk::Filter::eLinear,
                                   .minFilter = vk::Filter::eLinear,
                                   .mipmapMode = vk::SamplerMipmapMode::eLinear },
            4.0F,
            VK_TRUE,
            16.0F,
            vk::BorderColor::eFloatOpaqueBlack);
        expectSameFields(scalar, desc);
    }

    {
        // Non-default parameter set: nearest filtering, clamp-to-edge, comparison enabled.
        const vk::SamplerCreateInfo scalar = buildSamplerCreateInfo(vk::Filter::eNearest,
          vk::SamplerAddressMode::eClampToEdge,
          5.0F,
          VK_FALSE,
          1.0F,
          vk::BorderColor::eIntOpaqueBlack,
          VK_TRUE,
          vk::CompareOp::eLessOrEqual);
        const vk::SamplerCreateInfo desc =
          buildSamplerCreateInfo(GltfSamplerDesc{ .addressModeU = vk::SamplerAddressMode::eClampToEdge,
                                   .addressModeV = vk::SamplerAddressMode::eClampToEdge,
                                   .magFilter = vk::Filter::eNearest,
                                   .minFilter = vk::Filter::eNearest,
                                   .mipmapMode = vk::SamplerMipmapMode::eLinear },
            5.0F,
            VK_FALSE,
            1.0F,
            vk::BorderColor::eIntOpaqueBlack,
            VK_TRUE,
            vk::CompareOp::eLessOrEqual);
        expectSameFields(scalar, desc);
    }
}

TEST(SamplerBuilder, FindSamplerReusesAnEqualKey)
{
    const std::array<SamplerKey, 3> created{
        SamplerKey{ 1U, GltfSamplerDesc{} },
        SamplerKey{ 4U, GltfSamplerDesc{} },
        SamplerKey{ 7U, GltfSamplerDesc{} },
    };

    EXPECT_EQ(findSampler(created, SamplerKey{ 4U, GltfSamplerDesc{} }), 1U);
    EXPECT_EQ(findSampler(created, SamplerKey{ 1U, GltfSamplerDesc{} }), 0U);
    EXPECT_EQ(findSampler(created, SamplerKey{ 7U, GltfSamplerDesc{} }), 2U);
}

TEST(SamplerBuilder, FindSamplerReturnsNulloptForANewKey)
{
    const std::array<SamplerKey, 3> created{
        SamplerKey{ 1U, GltfSamplerDesc{} },
        SamplerKey{ 4U, GltfSamplerDesc{} },
        SamplerKey{ 7U, GltfSamplerDesc{} },
    };

    EXPECT_EQ(findSampler(created, SamplerKey{ 2U, GltfSamplerDesc{} }), std::nullopt);
    EXPECT_EQ(findSampler({}, SamplerKey{ 0U, GltfSamplerDesc{} }), std::nullopt);
}

TEST(SamplerBuilderUnit, SamplerDedupSeparatesDistinctWrapModes)
{
    // Same mip count, different wrap mode: must not collapse onto one vk::Sampler.
    GltfSamplerDesc repeatDesc{};
    GltfSamplerDesc clampDesc{};
    clampDesc.addressModeU = vk::SamplerAddressMode::eClampToEdge;

    const std::array<SamplerKey, 1> created{ SamplerKey{ 3U, repeatDesc } };

    EXPECT_EQ(findSampler(created, SamplerKey{ 3U, repeatDesc }), 0U);
    EXPECT_EQ(findSampler(created, SamplerKey{ 3U, clampDesc }), std::nullopt);
}

TEST(SamplerBuilderUnit, NearestFilteringDropsAnisotropy)
{
    EXPECT_FALSE(usesNearestFiltering(GltfSamplerDesc{}));

    GltfSamplerDesc magNearest{};
    magNearest.magFilter = vk::Filter::eNearest;
    EXPECT_TRUE(usesNearestFiltering(magNearest));

    GltfSamplerDesc minNearest{};
    minNearest.minFilter = vk::Filter::eNearest;
    EXPECT_TRUE(usesNearestFiltering(minNearest));

    GltfSamplerDesc mipNearest{};
    mipNearest.mipmapMode = vk::SamplerMipmapMode::eNearest;
    EXPECT_TRUE(usesNearestFiltering(mipNearest));
}
