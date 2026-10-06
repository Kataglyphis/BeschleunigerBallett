#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <vulkan/vulkan.hpp>

#include "RepoFiles.hpp"
#include "common/RenderPassHelper.hpp"

using Kataglyphis::buildRenderPassCreateInfo;

namespace {
constexpr std::array<vk::AttachmentDescription, 3> kThreeAttachments{ vk::AttachmentDescription{},
    vk::AttachmentDescription{},
    vk::AttachmentDescription{} };
constexpr std::array<vk::SubpassDescription, 1> kOneSubpass{ vk::SubpassDescription{} };
constexpr std::array<vk::SubpassDependency, 1> kOneDependency{ vk::SubpassDependency{} };
}// namespace

static_assert(buildRenderPassCreateInfo(std::span<const vk::AttachmentDescription>(kThreeAttachments),
                std::span<const vk::SubpassDescription>(kOneSubpass),
                std::span<const vk::SubpassDependency>(kOneDependency))
                  .attachmentCount
                == 3U,
  "buildRenderPassCreateInfo must be usable in a constant expression");

namespace {

TEST(RenderPassCreateHelperUnit, AttachmentCountIsDerivedFromTheSpan)
{
    const vk::RenderPassCreateInfo info =
      buildRenderPassCreateInfo(std::span<const vk::AttachmentDescription>(kThreeAttachments),
        std::span<const vk::SubpassDescription>(kOneSubpass),
        std::span<const vk::SubpassDependency>(kOneDependency));

    EXPECT_EQ(info.attachmentCount, 3U);
}

TEST(RenderPassCreateHelperUnit, SubpassAndDependencyCountsAreDerivedFromTheirSpans)
{
    // The DeferredRasterizer shape: two subpasses, three dependencies.
    const std::array<vk::SubpassDescription, 2> two_subpasses{ vk::SubpassDescription{}, vk::SubpassDescription{} };
    const std::array<vk::SubpassDependency, 3> three_dependencies{
        vk::SubpassDependency{}, vk::SubpassDependency{}, vk::SubpassDependency{}
    };

    const vk::RenderPassCreateInfo info =
      buildRenderPassCreateInfo(std::span<const vk::AttachmentDescription>(kThreeAttachments),
        std::span<const vk::SubpassDescription>(two_subpasses),
        std::span<const vk::SubpassDependency>(three_dependencies));

    EXPECT_EQ(info.subpassCount, 2U);
    EXPECT_EQ(info.dependencyCount, 3U);
}

TEST(RenderPassCreateHelperUnit, PointersPointAtTheCallersStorage)
{
    const vk::RenderPassCreateInfo info =
      buildRenderPassCreateInfo(std::span<const vk::AttachmentDescription>(kThreeAttachments),
        std::span<const vk::SubpassDescription>(kOneSubpass),
        std::span<const vk::SubpassDependency>(kOneDependency));

    EXPECT_EQ(info.pAttachments, kThreeAttachments.data());
    EXPECT_EQ(info.pSubpasses, kOneSubpass.data());
    EXPECT_EQ(info.pDependencies, kOneDependency.data());
}

TEST(RenderPassCreateHelperUnit, SingleAttachmentPassStillReportsOne)
{
    // The CascadedShadowMap shape: one depth attachment, no colour.
    const std::array<vk::AttachmentDescription, 1> one_attachment{ vk::AttachmentDescription{} };

    const vk::RenderPassCreateInfo info =
      buildRenderPassCreateInfo(std::span<const vk::AttachmentDescription>(one_attachment),
        std::span<const vk::SubpassDescription>(kOneSubpass),
        std::span<const vk::SubpassDependency>(kOneDependency));

    EXPECT_EQ(info.attachmentCount, 1U);
    EXPECT_EQ(info.pAttachments, one_attachment.data());
}

TEST(RenderPassCreateHelperUnit, FlagsAndPNextAreDefaultedSoCallersCanChainTheirOwn)
{
    // CascadedShadowMap chains multiview info through pNext, which is safe only if the helper leaves it unset.
    const vk::RenderPassCreateInfo info =
      buildRenderPassCreateInfo(std::span<const vk::AttachmentDescription>(kThreeAttachments),
        std::span<const vk::SubpassDescription>(kOneSubpass),
        std::span<const vk::SubpassDependency>(kOneDependency));

    EXPECT_EQ(info.flags, vk::RenderPassCreateFlags{});
    EXPECT_EQ(info.pNext, nullptr);
}

}// namespace

namespace {
namespace fs = std::filesystem;

using Kataglyphis::TestSupport::readFileText;
using Kataglyphis::TestSupport::repoRoot;

}// namespace

// A source gate: nothing reads a Post or SkyBox depth attachment, so neither pass may declare one.
TEST(RenderPassCreateHelperUnit, PostAndSkyboxPassesDeclareNoDepthAttachment)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path post_stage_path = repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "PostStage.cpp";
    const fs::path sky_box_path = repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "sky_box" / "SkyBox.cpp";

    const std::string post_stage_contents = readFileText(post_stage_path).value_or(std::string{});
    const std::string sky_box_contents = readFileText(sky_box_path).value_or(std::string{});
    ASSERT_FALSE(post_stage_contents.empty()) << "missing or empty " << post_stage_path.string();
    ASSERT_FALSE(sky_box_contents.empty()) << "missing or empty " << sky_box_path.string();

    EXPECT_EQ(post_stage_contents.find("eDepthStencilAttachmentWrite"), std::string::npos)
      << "PostStage.cpp still references eDepthStencilAttachmentWrite - see backlog entry "
         "\"Delete the depth attachment that PostStage and SkyBox allocate, clear and synchronize but never test "
         "or write\"";
    EXPECT_EQ(post_stage_contents.find("depthBufferImage"), std::string::npos)
      << "PostStage.cpp still references depthBufferImage - see the same backlog entry";
    EXPECT_EQ(post_stage_contents.find("depth_attachment"), std::string::npos)
      << "PostStage.cpp's render-pass creation still declares a depth_attachment - see the same backlog entry";

    EXPECT_EQ(sky_box_contents.find("eDepthStencilAttachmentWrite"), std::string::npos)
      << "SkyBox.cpp still references eDepthStencilAttachmentWrite - see the same backlog entry";
    EXPECT_EQ(sky_box_contents.find("depthAttachment"), std::string::npos)
      << "SkyBox.cpp's render-pass creation still declares a depthAttachment - see the same backlog entry";

    // Control: the rasterizers own real depth buffers and must keep synchronizing them.
    const fs::path rasterizer_path = repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "Rasterizer.cpp";
    const fs::path deferred_rasterizer_path =
      repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "DeferredRasterizer.cpp";

    const std::string rasterizer_contents = readFileText(rasterizer_path).value_or(std::string{});
    const std::string deferred_rasterizer_contents = readFileText(deferred_rasterizer_path).value_or(std::string{});
    ASSERT_FALSE(rasterizer_contents.empty()) << "missing or empty " << rasterizer_path.string();
    ASSERT_FALSE(deferred_rasterizer_contents.empty()) << "missing or empty " << deferred_rasterizer_path.string();

    EXPECT_NE(rasterizer_contents.find("eDepthStencilAttachmentOptimal"), std::string::npos)
      << "Rasterizer.cpp no longer declares a depth attachment - this gate must not pass by deleting depth "
         "synchronization everywhere";
    EXPECT_NE(deferred_rasterizer_contents.find("eDepthStencilAttachmentWrite"), std::string::npos)
      << "DeferredRasterizer.cpp no longer synchronizes its depth attachment - this gate must not pass by "
         "deleting depth synchronization everywhere";
}
