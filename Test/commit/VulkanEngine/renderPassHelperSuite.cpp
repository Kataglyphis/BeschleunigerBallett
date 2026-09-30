// No pixel oracle sees a stencil aspect left at eStore, so every call site is pinned field by field.

#include <gtest/gtest.h>

#include <array>
#include <vulkan/vulkan.hpp>

#include "common/RenderPassHelper.hpp"

using Kataglyphis::buildAttachmentDescription;
using Kataglyphis::buildExternalColorDepthDependency;
using Kataglyphis::destroyRenderPass;

// Usable in a constant expression, matching ViewportHelper.hpp's convention.
static_assert(
  buildAttachmentDescription(vk::Format::eR16G16B16A16Sfloat, vk::ImageLayout::eShaderReadOnlyOptimal).samples
    == vk::SampleCountFlagBits::e1,
  "buildAttachmentDescription must be usable in a constant expression");

namespace {

// Asserted per call site so no single site can drift from the engine-wide fields.
void expectEngineWideAttachmentInvariants(const vk::AttachmentDescription &description)
{
    EXPECT_EQ(description.samples, vk::SampleCountFlagBits::e1);
    EXPECT_EQ(description.stencilLoadOp, vk::AttachmentLoadOp::eDontCare);
    EXPECT_EQ(description.stencilStoreOp, vk::AttachmentStoreOp::eDontCare);
    EXPECT_EQ(description.flags, vk::AttachmentDescriptionFlags{});
}

TEST(RenderPassHelperUnit, DefaultsAreClearStoreFromUndefined)
{
    const vk::AttachmentDescription description =
      buildAttachmentDescription(vk::Format::eR8G8B8A8Unorm, vk::ImageLayout::eShaderReadOnlyOptimal);

    EXPECT_EQ(description.loadOp, vk::AttachmentLoadOp::eClear);
    EXPECT_EQ(description.storeOp, vk::AttachmentStoreOp::eStore);
    EXPECT_EQ(description.initialLayout, vk::ImageLayout::eUndefined);
    EXPECT_EQ(description.finalLayout, vk::ImageLayout::eShaderReadOnlyOptimal);
    EXPECT_EQ(description.format, vk::Format::eR8G8B8A8Unorm);
    expectEngineWideAttachmentInvariants(description);
}

TEST(RenderPassHelperUnit, MatchesForwardRasterizerColorAttachment)
{
    // Rasterizer's colour target, sampled by the post stage afterwards.
    const vk::AttachmentDescription description =
      buildAttachmentDescription(vk::Format::eR16G16B16A16Sfloat, vk::ImageLayout::eShaderReadOnlyOptimal);

    EXPECT_EQ(description.format, vk::Format::eR16G16B16A16Sfloat);
    EXPECT_EQ(description.loadOp, vk::AttachmentLoadOp::eClear);
    EXPECT_EQ(description.storeOp, vk::AttachmentStoreOp::eStore);
    EXPECT_EQ(description.initialLayout, vk::ImageLayout::eUndefined);
    EXPECT_EQ(description.finalLayout, vk::ImageLayout::eShaderReadOnlyOptimal);
    expectEngineWideAttachmentInvariants(description);
}

TEST(RenderPassHelperUnit, MatchesForwardRasterizerDepthAttachmentWithDiscardedStore)
{
    // Nothing reads Rasterizer's depth afterwards; losing the eDontCare store would cost bandwidth silently.
    const vk::AttachmentDescription description = buildAttachmentDescription(vk::Format::eD32Sfloat,
      vk::ImageLayout::eDepthStencilAttachmentOptimal,
      vk::AttachmentLoadOp::eClear,
      vk::AttachmentStoreOp::eDontCare);

    EXPECT_EQ(description.loadOp, vk::AttachmentLoadOp::eClear);
    EXPECT_EQ(description.storeOp, vk::AttachmentStoreOp::eDontCare);
    EXPECT_EQ(description.initialLayout, vk::ImageLayout::eUndefined);
    EXPECT_EQ(description.finalLayout, vk::ImageLayout::eDepthStencilAttachmentOptimal);
    expectEngineWideAttachmentInvariants(description);
}

TEST(RenderPassHelperUnit, MatchesDeferredGBufferAttachments)
{
    const std::array<vk::AttachmentDescription, 5> attachments = {
        buildAttachmentDescription(vk::Format::eR16G16B16A16Sfloat, vk::ImageLayout::eShaderReadOnlyOptimal),
        buildAttachmentDescription(vk::Format::eR16G16B16A16Sfloat, vk::ImageLayout::eShaderReadOnlyOptimal),
        buildAttachmentDescription(vk::Format::eR8G8B8A8Unorm, vk::ImageLayout::eShaderReadOnlyOptimal),
        buildAttachmentDescription(vk::Format::eR8G8B8A8Unorm, vk::ImageLayout::eShaderReadOnlyOptimal),
        buildAttachmentDescription(vk::Format::eD32Sfloat, vk::ImageLayout::eDepthStencilAttachmentOptimal)
    };

    for (const vk::AttachmentDescription &description : attachments) {
        expectEngineWideAttachmentInvariants(description);
        EXPECT_EQ(description.loadOp, vk::AttachmentLoadOp::eClear);
        EXPECT_EQ(description.storeOp, vk::AttachmentStoreOp::eStore);
        EXPECT_EQ(description.initialLayout, vk::ImageLayout::eUndefined);
    }

    EXPECT_EQ(attachments[4].finalLayout, vk::ImageLayout::eDepthStencilAttachmentOptimal);
    EXPECT_EQ(attachments[0].finalLayout, vk::ImageLayout::eShaderReadOnlyOptimal);
}

TEST(RenderPassHelperUnit, MatchesSkyBoxColorAttachmentLeftForThePostStageToLoad)
{
    // PostStage loads the swapchain image out of exactly the layout SkyBox leaves it in.
    const vk::AttachmentDescription sky_box = buildAttachmentDescription(
      vk::Format::eB8G8R8A8Unorm, vk::ImageLayout::eColorAttachmentOptimal);

    const vk::AttachmentDescription post_stage = buildAttachmentDescription(vk::Format::eB8G8R8A8Unorm,
      vk::ImageLayout::ePresentSrcKHR,
      vk::AttachmentLoadOp::eLoad,
      vk::AttachmentStoreOp::eStore,
      vk::ImageLayout::eColorAttachmentOptimal);

    EXPECT_EQ(sky_box.finalLayout, post_stage.initialLayout);
    EXPECT_EQ(sky_box.loadOp, vk::AttachmentLoadOp::eClear);
    EXPECT_EQ(sky_box.initialLayout, vk::ImageLayout::eUndefined);
    expectEngineWideAttachmentInvariants(sky_box);
}

TEST(RenderPassHelperUnit, MatchesPostStageColorAttachmentOverridingEveryDefault)
{
    // The one call site that overrides all three defaulted parameters.
    const vk::AttachmentDescription description = buildAttachmentDescription(vk::Format::eB8G8R8A8Unorm,
      vk::ImageLayout::ePresentSrcKHR,
      vk::AttachmentLoadOp::eLoad,
      vk::AttachmentStoreOp::eStore,
      vk::ImageLayout::eColorAttachmentOptimal);

    EXPECT_EQ(description.loadOp, vk::AttachmentLoadOp::eLoad);
    EXPECT_EQ(description.storeOp, vk::AttachmentStoreOp::eStore);
    EXPECT_EQ(description.initialLayout, vk::ImageLayout::eColorAttachmentOptimal);
    EXPECT_EQ(description.finalLayout, vk::ImageLayout::ePresentSrcKHR);
    expectEngineWideAttachmentInvariants(description);
}

TEST(RenderPassHelperUnit, MatchesPostStageDepthAttachmentThatNeverLeavesItsLayout)
{
    const vk::AttachmentDescription description = buildAttachmentDescription(vk::Format::eD32SfloatS8Uint,
      vk::ImageLayout::eDepthStencilAttachmentOptimal,
      vk::AttachmentLoadOp::eClear,
      vk::AttachmentStoreOp::eDontCare,
      vk::ImageLayout::eDepthStencilAttachmentOptimal);

    EXPECT_EQ(description.initialLayout, description.finalLayout);
    EXPECT_EQ(description.storeOp, vk::AttachmentStoreOp::eDontCare);
    // The engine has no stencil pass, so even a stencil format declares eDontCare.
    expectEngineWideAttachmentInvariants(description);
}

TEST(RenderPassHelperUnit, MatchesCascadedShadowMapDepthAttachmentThatIsSampledLater)
{
    // The only stored depth attachment, because the lighting shader samples it.
    const vk::AttachmentDescription description =
      buildAttachmentDescription(vk::Format::eD32Sfloat, vk::ImageLayout::eShaderReadOnlyOptimal);

    EXPECT_EQ(description.storeOp, vk::AttachmentStoreOp::eStore);
    EXPECT_EQ(description.loadOp, vk::AttachmentLoadOp::eClear);
    EXPECT_EQ(description.initialLayout, vk::ImageLayout::eUndefined);
    EXPECT_EQ(description.finalLayout, vk::ImageLayout::eShaderReadOnlyOptimal);
    expectEngineWideAttachmentInvariants(description);
}

TEST(RenderPassHelper, ExternalDependencyCoversDepthWrites)
{
    // One depth image spans frames in flight, so without this the next clear races the last store.
    const vk::SubpassDependency dependency = buildExternalColorDepthDependency();

    EXPECT_EQ(dependency.srcSubpass, VK_SUBPASS_EXTERNAL);
    EXPECT_EQ(dependency.dstSubpass, 0U);
    EXPECT_TRUE(dependency.srcStageMask & vk::PipelineStageFlagBits::eEarlyFragmentTests);
    EXPECT_TRUE(dependency.srcStageMask & vk::PipelineStageFlagBits::eLateFragmentTests);
    EXPECT_TRUE(dependency.srcAccessMask & vk::AccessFlagBits::eDepthStencilAttachmentWrite);
    EXPECT_TRUE(dependency.dstAccessMask & vk::AccessFlagBits::eDepthStencilAttachmentWrite);
    // Not by-region: this is a cross-frame dependency, not a tile-local one.
    EXPECT_EQ(dependency.dependencyFlags, vk::DependencyFlags{});
}

TEST(RenderPassHelperUnit, DestroyRenderPassOnANullDeviceIsANoOp)
{
    // Without a device, destroyRenderPass must not touch even a non-null-looking handle.
    vk::RenderPass render_pass(reinterpret_cast<VkRenderPass>(0x1));
    destroyRenderPass(vk::Device{}, render_pass);
    EXPECT_EQ(render_pass, vk::RenderPass(reinterpret_cast<VkRenderPass>(0x1)));
}

TEST(RenderPassHelperUnit, DestroyRenderPassOnANullHandleIsANoOp)
{
    vk::RenderPass render_pass(nullptr);
    destroyRenderPass(vk::Device{}, render_pass);
    EXPECT_EQ(render_pass, vk::RenderPass(nullptr));
}

}// namespace
