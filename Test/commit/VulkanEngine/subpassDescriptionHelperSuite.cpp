#include <gtest/gtest.h>

#include <array>
#include <span>
#include <vulkan/vulkan.hpp>

#include "common/RenderPassHelper.hpp"

using Kataglyphis::buildSubpassDescription;

namespace {
constexpr vk::AttachmentReference kColorRef{ 0, vk::ImageLayout::eColorAttachmentOptimal };
constexpr vk::AttachmentReference kDepthRef{ 1, vk::ImageLayout::eDepthStencilAttachmentOptimal };
constexpr std::array<vk::AttachmentReference, 3> kThreeColorRefs{ vk::AttachmentReference{ 1,
                                                                    vk::ImageLayout::eColorAttachmentOptimal },
    vk::AttachmentReference{ 2, vk::ImageLayout::eColorAttachmentOptimal },
    vk::AttachmentReference{ 3, vk::ImageLayout::eColorAttachmentOptimal } };
constexpr std::array<vk::AttachmentReference, 4> kFourInputRefs{ vk::AttachmentReference{ 1,
                                                                   vk::ImageLayout::eShaderReadOnlyOptimal },
    vk::AttachmentReference{ 2, vk::ImageLayout::eShaderReadOnlyOptimal },
    vk::AttachmentReference{ 3, vk::ImageLayout::eShaderReadOnlyOptimal },
    vk::AttachmentReference{ 4, vk::ImageLayout::eShaderReadOnlyOptimal } };
}// namespace

static_assert(
  buildSubpassDescription(std::span<const vk::AttachmentReference>(&kColorRef, 1), &kDepthRef).colorAttachmentCount
    == 1U,
  "buildSubpassDescription must be usable in a constant expression");

namespace {

TEST(SubpassDescriptionHelperUnit, ColorAttachmentCountIsDerivedFromASingleElementSpan)
{
    // The Rasterizer / PostStage / SkyBox shape: one colour ref, one depth ref.
    const vk::SubpassDescription subpass =
      buildSubpassDescription(std::span<const vk::AttachmentReference>(&kColorRef, 1), &kDepthRef);

    EXPECT_EQ(subpass.colorAttachmentCount, 1U);
    EXPECT_EQ(subpass.pColorAttachments, &kColorRef);
}

TEST(SubpassDescriptionHelperUnit, ColorAttachmentCountIsDerivedFromAThreeElementSpan)
{
    // The DeferredRasterizer geometry-subpass shape: normal, albedo, material.
    const vk::SubpassDescription subpass =
      buildSubpassDescription(std::span<const vk::AttachmentReference>(kThreeColorRefs), &kDepthRef);

    EXPECT_EQ(subpass.colorAttachmentCount, 3U);
    EXPECT_EQ(subpass.pColorAttachments, kThreeColorRefs.data());
}

TEST(SubpassDescriptionHelperUnit, ColorAttachmentCountIsZeroForAnEmptySpan)
{
    // The CascadedShadowMap shape: depth-only, no colour attachment at all.
    const vk::SubpassDescription subpass =
      buildSubpassDescription(std::span<const vk::AttachmentReference>{}, &kDepthRef);

    EXPECT_EQ(subpass.colorAttachmentCount, 0U);
}

TEST(SubpassDescriptionHelperUnit, DepthStencilAttachmentPointerRoundTrips)
{
    const vk::SubpassDescription subpass =
      buildSubpassDescription(std::span<const vk::AttachmentReference>(&kColorRef, 1), &kDepthRef);

    EXPECT_EQ(subpass.pDepthStencilAttachment, &kDepthRef);
}

TEST(SubpassDescriptionHelperUnit, DepthStencilAttachmentIsNullWhenNoneIsPassed)
{
    // The deferred lighting subpass has no depth attachment of its own.
    const vk::SubpassDescription subpass =
      buildSubpassDescription(std::span<const vk::AttachmentReference>(&kColorRef, 1),
        nullptr,
        std::span<const vk::AttachmentReference>(kFourInputRefs));

    EXPECT_EQ(subpass.pDepthStencilAttachment, nullptr);
}

TEST(SubpassDescriptionHelperUnit, InputAttachmentCountIsZeroWhenTheDefaultedArgumentIsOmitted)
{
    const vk::SubpassDescription subpass =
      buildSubpassDescription(std::span<const vk::AttachmentReference>(&kColorRef, 1), &kDepthRef);

    EXPECT_EQ(subpass.inputAttachmentCount, 0U);
    EXPECT_EQ(subpass.pInputAttachments, nullptr);
}

TEST(SubpassDescriptionHelperUnit, InputAttachmentCountIsFourForTheDeferredLightingShape)
{
    const vk::SubpassDescription subpass =
      buildSubpassDescription(std::span<const vk::AttachmentReference>(&kColorRef, 1),
        nullptr,
        std::span<const vk::AttachmentReference>(kFourInputRefs));

    EXPECT_EQ(subpass.inputAttachmentCount, 4U);
    EXPECT_EQ(subpass.pInputAttachments, kFourInputRefs.data());
}

TEST(SubpassDescriptionHelperUnit, PipelineBindPointIsAlwaysGraphics)
{
    const vk::SubpassDescription subpass =
      buildSubpassDescription(std::span<const vk::AttachmentReference>(&kColorRef, 1), &kDepthRef);

    EXPECT_EQ(subpass.pipelineBindPoint, vk::PipelineBindPoint::eGraphics);
}

TEST(SubpassDescriptionHelperUnit, ResolveAttachmentsAreLeftNull)
{
    const vk::SubpassDescription subpass =
      buildSubpassDescription(std::span<const vk::AttachmentReference>(&kColorRef, 1), &kDepthRef);

    EXPECT_EQ(subpass.pResolveAttachments, nullptr);
}

TEST(SubpassDescriptionHelperUnit, MatchesRasterizerSubpass)
{
    // Rasterizer::createRenderPass: colour at 0, depth at 1, no input attachments.
    constexpr vk::AttachmentReference color_ref{ 0, vk::ImageLayout::eColorAttachmentOptimal };
    constexpr vk::AttachmentReference depth_ref{ 1, vk::ImageLayout::eDepthStencilAttachmentOptimal };

    const vk::SubpassDescription subpass =
      buildSubpassDescription(std::span<const vk::AttachmentReference>(&color_ref, 1), &depth_ref);

    EXPECT_EQ(subpass.colorAttachmentCount, 1U);
    EXPECT_EQ(subpass.pDepthStencilAttachment, &depth_ref);
    EXPECT_EQ(subpass.inputAttachmentCount, 0U);
    EXPECT_EQ(subpass.pipelineBindPoint, vk::PipelineBindPoint::eGraphics);
}

TEST(SubpassDescriptionHelperUnit, MatchesPostStageSubpass)
{
    // Rasterizer's shape, over the swapchain target and a reused depth buffer.
    constexpr vk::AttachmentReference color_ref{ 0, vk::ImageLayout::eColorAttachmentOptimal };
    constexpr vk::AttachmentReference depth_ref{ 1, vk::ImageLayout::eDepthStencilAttachmentOptimal };

    const vk::SubpassDescription subpass =
      buildSubpassDescription(std::span<const vk::AttachmentReference>(&color_ref, 1), &depth_ref);

    EXPECT_EQ(subpass.colorAttachmentCount, 1U);
    EXPECT_EQ(subpass.pDepthStencilAttachment, &depth_ref);
    EXPECT_EQ(subpass.inputAttachmentCount, 0U);
}

TEST(SubpassDescriptionHelperUnit, MatchesSkyBoxSubpass)
{
    // SkyBox::createRenderPass: colour at 0, depth at 1, no input attachments.
    constexpr vk::AttachmentReference color_ref{ 0, vk::ImageLayout::eColorAttachmentOptimal };
    constexpr vk::AttachmentReference depth_ref{ 1, vk::ImageLayout::eDepthStencilAttachmentOptimal };

    const vk::SubpassDescription subpass =
      buildSubpassDescription(std::span<const vk::AttachmentReference>(&color_ref, 1), &depth_ref);

    EXPECT_EQ(subpass.colorAttachmentCount, 1U);
    EXPECT_EQ(subpass.pDepthStencilAttachment, &depth_ref);
    EXPECT_EQ(subpass.inputAttachmentCount, 0U);
}

TEST(SubpassDescriptionHelperUnit, MatchesCascadedShadowMapSubpass)
{
    // The only call site with an empty colour span.
    constexpr vk::AttachmentReference depth_ref{ 0, vk::ImageLayout::eDepthStencilAttachmentOptimal };

    const vk::SubpassDescription subpass =
      buildSubpassDescription(std::span<const vk::AttachmentReference>{}, &depth_ref);

    EXPECT_EQ(subpass.colorAttachmentCount, 0U);
    EXPECT_EQ(subpass.pDepthStencilAttachment, &depth_ref);
    EXPECT_EQ(subpass.inputAttachmentCount, 0U);
}

TEST(SubpassDescriptionHelperUnit, MatchesDeferredRasterizerGeometrySubpass)
{
    // The only three-element colour span: the G-buffer.
    const vk::SubpassDescription subpass =
      buildSubpassDescription(std::span<const vk::AttachmentReference>(kThreeColorRefs), &kDepthRef);

    EXPECT_EQ(subpass.colorAttachmentCount, 3U);
    EXPECT_EQ(subpass.pColorAttachments, kThreeColorRefs.data());
    EXPECT_EQ(subpass.pDepthStencilAttachment, &kDepthRef);
    EXPECT_EQ(subpass.inputAttachmentCount, 0U);
}

TEST(SubpassDescriptionHelperUnit, MatchesDeferredRasterizerLightingSubpass)
{
    // Reads the G-buffers and depth back as input attachments, so it has no depth attachment of its own.
    const vk::SubpassDescription subpass =
      buildSubpassDescription(std::span<const vk::AttachmentReference>(&kColorRef, 1),
        nullptr,
        std::span<const vk::AttachmentReference>(kFourInputRefs));

    EXPECT_EQ(subpass.colorAttachmentCount, 1U);
    EXPECT_EQ(subpass.pDepthStencilAttachment, nullptr);
    EXPECT_EQ(subpass.inputAttachmentCount, 4U);
    EXPECT_EQ(subpass.pInputAttachments, kFourInputRefs.data());
}

}// namespace
