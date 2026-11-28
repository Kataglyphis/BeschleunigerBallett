// Build integrity: barriers name the stage that consumes them, and submits wait where an image is first used.

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <map>
#include <regex>
#include <string>
#include <vector>

#include "BuildIntegrityScan.hpp"

using namespace Kataglyphis::TestSupport::BuildIntegrity;

// Default sync validation sees neither edge: it skips the traced sky's storage-to-attachment hand-off and shader reads.
TEST(BuildIntegrity, PostKeepsItsSkyAndCloudBarriers)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    // The traced sky reads the trace's alpha through its blend: RAW from a storage write to an attachment read.
    const auto sky = readFileText(repo_root / "Src/GraphicsEngineVulkan/scene/sky_box/SkyBox.cpp");
    ASSERT_TRUE(sky.has_value()) << "could not open SkyBox.cpp";
    const auto traced = function_body_span(*sky, "SkyBox::recordTracedDraw");
    ASSERT_TRUE(traced.has_value()) << "SkyBox::recordTracedDraw not found";
    const std::size_t begin_rendering = sky->find("beginRendering(", traced->first);
    ASSERT_TRUE(begin_rendering != std::string::npos && begin_rendering < traced->second)
      << "SkyBox::recordTracedDraw begins no rendering";
    const std::string before_load = sky->substr(traced->first, begin_rendering - traced->first);
    EXPECT_NE(before_load.find("vk::AccessFlagBits2::eShaderStorageWrite"), std::string::npos)
      << "the traced sky's pre-load barrier must make the trace's storage write available";
    EXPECT_NE(before_load.find("vk::AccessFlagBits2::eColorAttachmentRead"), std::string::npos)
      << "the traced sky's pre-load barrier must make the trace visible to the load op and the blend's read";
    const std::string after_draw = sky->substr(begin_rendering, traced->second - begin_rendering);
    EXPECT_NE(after_draw.find("vk::ImageLayout::eShaderReadOnlyOptimal"), std::string::npos)
      << "the traced sky must hand its target to post's fragment read";

    const auto renderer = readFileText(repo_root / "Src/GraphicsEngineVulkan/renderer/VulkanRenderer.cpp");
    ASSERT_TRUE(renderer.has_value()) << "could not open VulkanRenderer.cpp";
    // The clouds stage ends where its record hook does; the registry times it, so no timestamp marks the end.
    const auto clouds_stage = function_body_span(*renderer, "Kataglyphis::VulkanRenderer::recordCloudsStage");
    ASSERT_TRUE(clouds_stage.has_value()) << "VulkanRenderer::recordCloudsStage was not found";
    const std::size_t dispatch = renderer->find("clouds.recordComputeCommands(", clouds_stage->first);
    ASSERT_TRUE(dispatch != std::string::npos && dispatch < clouds_stage->second)
      << "the clouds' compute dispatch is not in recordCloudsStage";
    const std::string after_dispatch = renderer->substr(dispatch, clouds_stage->second - dispatch);
    EXPECT_NE(after_dispatch.find("getCloudOutputTexture()"), std::string::npos)
      << "nothing orders the clouds' compute write before post's fragment read of the cloud output";
    EXPECT_NE(after_dispatch.find("vk::AccessFlagBits2::eShaderStorageWrite"), std::string::npos)
      << "the clouds-to-post barrier must make the compute shader's storage write available";
    EXPECT_NE(after_dispatch.find("vk::PipelineStageFlagBits2::eFragmentShader"), std::string::npos)
      << "the clouds-to-post barrier must name post's fragment shader as its consumer";
    EXPECT_NE(after_dispatch.find("getCloudHalfTexture()"), std::string::npos)
      << "nothing orders the half-resolution march's write before the upsample's read of it";
    EXPECT_NE(after_dispatch.find("vk::AccessFlagBits2::eShaderSampledRead"), std::string::npos)
      << "the march's closing barriers must make its write visible to sampled reads";

    // At half resolution the upsample writes what post reads, so the clouds-to-post edge is its too.
    const auto upsample_stage = function_body_span(*renderer, "Kataglyphis::VulkanRenderer::recordCloudUpsampleStage");
    ASSERT_TRUE(upsample_stage.has_value()) << "VulkanRenderer::recordCloudUpsampleStage was not found";
    const std::size_t upsample = renderer->find("clouds.recordUpsampleCommands(", upsample_stage->first);
    ASSERT_TRUE(upsample != std::string::npos && upsample < upsample_stage->second)
      << "the clouds' upsample dispatch is not in recordCloudUpsampleStage";
    const std::string before_upsample = renderer->substr(upsample_stage->first, upsample - upsample_stage->first);
    EXPECT_NE(before_upsample.find("getCloudOutputTexture()"), std::string::npos)
      << "nothing orders the previous frame's post read of the cloud output before the upsample writes it";
    const std::string after_upsample = renderer->substr(upsample, upsample_stage->second - upsample);
    EXPECT_NE(after_upsample.find("getCloudOutputTexture()"), std::string::npos)
      << "nothing orders the upsample's compute write before post's fragment read of the cloud output";
    EXPECT_NE(after_upsample.find("vk::AccessFlagBits2::eShaderStorageWrite"), std::string::npos)
      << "the upsample-to-post barrier must make the compute shader's storage write available";
    EXPECT_NE(after_upsample.find("vk::PipelineStageFlagBits2::eFragmentShader"), std::string::npos)
      << "the upsample-to-post barrier must name post's fragment shader as its consumer";
}

// The acquire wait and post's first barrier share one constant, and the signal covers the present transition.
TEST(BuildIntegrity, FrameSubmitWaitsWhereTheSwapchainImageIsFirstUsed)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const auto renderer = readFileText(repo_root / "Src/GraphicsEngineVulkan/renderer/VulkanRenderer.cpp");
    ASSERT_TRUE(renderer.has_value()) << "could not open VulkanRenderer.cpp";
    EXPECT_NE(renderer->find("getGraphicsQueue().submit2("), std::string::npos)
      << "the frame submit must be vkQueueSubmit2, so its semaphore stages are synchronization2 stages";
    EXPECT_EQ(renderer->find("pWaitDstStageMask"), std::string::npos)
      << "a legacy VkSubmitInfo wait stage is back in VulkanRenderer.cpp";
    EXPECT_NE(
      renderer->find("frameSync.imageAvailableSemaphore(), Kataglyphis::kSwapchainAcquireWaitStage"), std::string::npos)
      << "the acquire semaphore must be waited at kSwapchainAcquireWaitStage";
    EXPECT_NE(renderer->find("Kataglyphis::kRenderFinishedSignalStage"), std::string::npos)
      << "the render-finished semaphore must be signalled at kRenderFinishedSignalStage";

    const auto post = readFileText(repo_root / "Src/GraphicsEngineVulkan/renderer/PostStage.cpp");
    ASSERT_TRUE(post.has_value()) << "could not open PostStage.cpp";
    const std::size_t record = post->find("PostStage::recordCommands(");
    ASSERT_NE(record, std::string::npos) << "PostStage::recordCommands not found";
    const std::size_t begin_rendering = post->find("beginRendering(", record);
    ASSERT_NE(begin_rendering, std::string::npos) << "PostStage::recordCommands begins no rendering";
    EXPECT_NE(post->substr(record, begin_rendering - record).find("kSwapchainAcquireWaitStage"), std::string::npos)
      << "post's UNDEFINED transition of the swapchain image must take its source stage from the acquire wait, "
         "or the transition can run before the presentation engine releases the image";

    // While FXAA filters the frame, its stage is the swapchain image's first user instead.
    const auto fxaa = readFileText(repo_root / "Src/GraphicsEngineVulkan/renderer/FxaaStage.cpp");
    ASSERT_TRUE(fxaa.has_value()) << "could not open FxaaStage.cpp";
    const std::size_t fxaa_record = fxaa->find("FxaaStage::recordCommands(");
    ASSERT_NE(fxaa_record, std::string::npos) << "FxaaStage::recordCommands not found";
    const std::size_t fxaa_rendering = fxaa->find("beginRendering(", fxaa_record);
    ASSERT_NE(fxaa_rendering, std::string::npos) << "FxaaStage::recordCommands begins no rendering";
    EXPECT_NE(
      fxaa->substr(fxaa_record, fxaa_rendering - fxaa_record).find("kSwapchainAcquireWaitStage"), std::string::npos)
      << "FXAA's UNDEFINED transition of the swapchain image must take its source stage from the acquire wait";
}

// Edges no frame validates: the TLAS's instance read and a re-armed capture writing a buffer a running frame writes.
TEST(BuildIntegrity, OneShotCopiesNameTheAccessesTheSpecRequires)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const auto as_manager =
      readFileText(repo_root / "Src/GraphicsEngineVulkan/renderer/accelerationStructures/ASManager.cpp");
    ASSERT_TRUE(as_manager.has_value()) << "could not open ASManager.cpp";
    const std::size_t create_tlas = as_manager->find("ASManager::createTLAS(");
    ASSERT_NE(create_tlas, std::string::npos) << "ASManager::createTLAS not found";
    const std::size_t upload_barrier = as_manager->find("recordMemoryBarrier(", create_tlas);
    ASSERT_NE(upload_barrier, std::string::npos) << "createTLAS records no global barrier for its instance upload";
    const std::size_t call_end = as_manager->find(");", upload_barrier);
    ASSERT_NE(call_end, std::string::npos) << "the instance upload's barrier call does not end";
    const std::string call = as_manager->substr(upload_barrier, call_end - upload_barrier);
    ASSERT_NE(call.find("vk::AccessFlagBits2::eTransferWrite"), std::string::npos)
      << "createTLAS's first global barrier is not the instance upload's: " << call;
    // (commandBuffer, srcStage, srcAccess, dstStage, dstAccess): the last argument is what the build reads.
    EXPECT_NE(call.substr(call.rfind(',')).find("eShaderRead"), std::string::npos)
      << "a TLAS build reads its instance buffer as VK_ACCESS_2_SHADER_READ_BIT, so the upload's barrier must name it";

    const auto capture = readFileText(repo_root / "Src/GraphicsEngineVulkan/renderer/FrameCapture.ixx");
    ASSERT_TRUE(capture.has_value()) << "could not open FrameCapture.ixx";
    const std::size_t record_copy = capture->find("void recordCopy(");
    ASSERT_NE(record_copy, std::string::npos) << "FrameCapture::recordCopy not found";
    const std::size_t copy = capture->find("copyImageToBuffer(", record_copy);
    ASSERT_NE(copy, std::string::npos) << "FrameCapture::recordCopy records no copy";
    EXPECT_NE(capture->substr(record_copy, copy - record_copy).find("pBufferMemoryBarriers"), std::string::npos)
      << "the capture copy must order itself after an earlier frame's copy into the same staging buffer (WAW)";
}

// A barrier into eShaderReadOnlyOptimal must name the stage that samples the image, or the real hazard goes unguarded.
TEST(BuildIntegrity, OffscreenImageBarriersNameTheStageThatConsumesThem)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const std::array<const char *, 3> kFiles = {
        "Src/GraphicsEngineVulkan/renderer/PathTracing.cpp",
        "Src/GraphicsEngineVulkan/renderer/Raytracing.cpp",
        "Src/GraphicsEngineVulkan/scene/sky_box/SkyBox.cpp",
    };

    // Sync2: (image, oldLayout, newLayout, srcStage, srcAccess, dstStage, dstAccess, ...), split at top-level commas.
    static const std::string kCall = "buildImageMemoryBarrier(";
    const auto split_call_arguments = [](const std::string &text, std::size_t open_paren) {
        std::vector<std::string> args;
        std::string current;
        int depth = 0;
        for (std::size_t i = open_paren; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '(' || c == '{') {
                if (depth++ > 0) { current += c; }
                continue;
            }
            if (c == ')' || c == '}') {
                if (--depth == 0) {
                    args.push_back(current);
                    break;
                }
                current += c;
                continue;
            }
            if (c == ',' && depth == 1) {
                args.push_back(current);
                current.clear();
                continue;
            }
            current += c;
        }
        return args;
    };

    std::size_t gated_barriers_found = 0;
    std::vector<std::string> violations;

    for (const char *relative_path : kFiles) {
        const fs::path path = repo_root / relative_path;
        const auto contentsOpt = readFileText(path);
        ASSERT_TRUE(contentsOpt.has_value()) << "missing " << path.string();
        const std::string &contents = *contentsOpt;

        for (std::size_t at = contents.find(kCall); at != std::string::npos; at = contents.find(kCall, at + 1)) {
            const std::vector<std::string> args = split_call_arguments(contents, at + kCall.size() - 1);
            ASSERT_GE(args.size(), 7u)
              << relative_path
              << ": buildImageMemoryBarrier call does not have the expected (image, oldLayout, newLayout, srcStage, "
                 "srcAccess, dstStage, dstAccess, ...) shape - the scan needs updating";

            if (args[2].find("eShaderReadOnlyOptimal") == std::string::npos) { continue; }

            ++gated_barriers_found;
            if (args[5].find("eFragmentShader") == std::string::npos) {
                violations.push_back(std::string(relative_path)
                                     + ": a barrier transitions to eShaderReadOnlyOptimal but names dst stage '"
                                     + args[5] + "' instead of eFragmentShader");
            }
        }
    }

    ASSERT_GT(gated_barriers_found, 0u) << "found zero image barriers transitioning to eShaderReadOnlyOptimal in "
                                           "PathTracing.cpp/Raytracing.cpp/SkyBox.cpp - the scan itself is broken";

    EXPECT_TRUE(violations.empty()) << joinViolations(violations);
}

// Image barriers come from buildImageMemoryBarrier; only empty-brace default construction is flagged, not the type.
TEST(BuildIntegrity, ImageMemoryBarriersGoThroughTheSharedHelper)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(sourceExists(src_root)) << "missing " << src_root.string();

    // Per-file budgets rather than exemptions; every unlisted file has a budget of 0.
    static const std::map<std::string, std::size_t> kBarrierBudgets = {
        { "Src/GraphicsEngineVulkan/scene/Texture.cpp", 1 },
    };

    const std::regex hand_rolled_barrier(R"(vk::ImageMemoryBarrier2\s+\w+\s*\{\s*\}\s*;)");

    std::map<std::string, std::size_t> counts;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx" && path.extension() != ".hpp") { continue; }

        const std::string relative_file = fs::relative(path, repo_root).generic_string();

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        for (const auto &line : *lines) {
            if (!std::regex_search(line, hand_rolled_barrier)) { continue; }
            ++counts[relative_file];
        }
    }

    std::vector<std::string> violations;
    for (const auto &[file, count] : counts) {
        const auto budget_it = kBarrierBudgets.find(file);
        const std::size_t budget = budget_it == kBarrierBudgets.end() ? 0 : budget_it->second;
        if (count == budget) { continue; }
        if (count > budget) {
            violations.push_back(file + ": found " + std::to_string(count) + " hand-rolled barrier(s), budget is "
                                  + std::to_string(budget)
                                  + " - route the new one(s) through Kataglyphis::buildImageMemoryBarrier "
                                    "(common/ImageBarrierHelper.hpp)");
        } else {
            violations.push_back(file + ": found " + std::to_string(count) + " hand-rolled barrier(s), budget is "
                                 + std::to_string(budget) + " - lower the budget in this test");
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " budget mismatch(es) for hand-rolled vk::ImageMemoryBarrier2 declarations under "
         "Src/GraphicsEngineVulkan/:"
      << joinViolations(violations);
}

// A global barrier is one call of recordMemoryBarrier, as an image barrier is one of buildImageMemoryBarrier.
TEST(BuildIntegrity, GlobalMemoryBarriersGoThroughTheSharedHelper)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(sourceExists(src_root)) << "missing " << src_root.string();
    static const std::string kHelper = "Src/GraphicsEngineVulkan/common/ImageBarrierHelper.hpp";
    static const std::string kHandRolled = "vk::MemoryBarrier2";

    std::vector<std::string> violations;
    std::size_t helper_declarations = 0;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx" && path.extension() != ".hpp") { continue; }
        const std::string relative_file = fs::relative(path, repo_root).generic_string();
        const auto text = readFileText(path);
        if (!text) { continue; }
        std::size_t count = 0;
        for (std::size_t at = text->find(kHandRolled); at != std::string::npos; at = text->find(kHandRolled, at + 1)) {
            ++count;
        }
        if (relative_file == kHelper) {
            helper_declarations = count;
        } else if (count > 0) {
            violations.push_back(relative_file + ": " + std::to_string(count)
                                 + " vk::MemoryBarrier2 - record it with Kataglyphis::recordMemoryBarrier");
        }
    }
    EXPECT_EQ(helper_declarations, 1U) << "recordMemoryBarrier no longer builds the one vk::MemoryBarrier2 - the scan "
                                          "needs updating";
    EXPECT_TRUE(violations.empty()) << joinViolations(violations);
}

TEST(BuildIntegrity, BarrierBudgetsNameOnlyFilesThatStillHaveBarriers)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const std::map<std::string, std::size_t> kBarrierBudgets = {
        { "Src/GraphicsEngineVulkan/scene/Texture.cpp", 1 },
    };

    const std::regex hand_rolled_barrier(R"(vk::ImageMemoryBarrier2\s+\w+\s*\{\s*\}\s*;)");

    for (const auto &[file, budget] : kBarrierBudgets) {
        EXPECT_GT(budget, 0u) << file << ": a budgeted file must have a non-zero budget, or it is a dead entry";

        const fs::path path = repo_root / file;
        ASSERT_TRUE(sourceExists(path)) << "budgeted file no longer exists: " << file;

        const auto lines = readFileLines(path);
        ASSERT_TRUE(lines.has_value()) << "could not read " << file;
        std::size_t count = 0;
        for (const auto &line : *lines) {
            if (std::regex_search(line, hand_rolled_barrier)) { ++count; }
        }
        EXPECT_EQ(count, budget) << file << ": budget says " << budget << " but the file currently has " << count;
    }
}

// RT and compute also sample the mips, so an eFragmentShader literal in Texture.cpp would narrow the stage.
TEST(BuildIntegrity, TextureUploadDoesNotNarrowItsShaderReadStage)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path texture_path = repo_root / "Src/GraphicsEngineVulkan/scene/Texture.cpp";
    const auto texture_text = readFileText(texture_path);
    ASSERT_TRUE(texture_text.has_value()) << "could not open " << texture_path.string();

    EXPECT_EQ(texture_text->find("eFragmentShader"), std::string::npos)
      << "Texture.cpp reintroduced eFragmentShader as a barrier destination stage - route "
         "eShaderReadOnlyOptimal transitions through Kataglyphis::pipelineStageForLayout instead, "
         "or the compute/ray-tracing readers of the mip chain lose synchronization.";
}

// Post overwrites every pixel, so a load or a clear is dead work; fs_main samples noisyTxt exactly once.
TEST(BuildIntegrity, TheOverwritingPostPassNeitherLoadsNorClears)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path post_stage_path = repo_root / "Src/GraphicsEngineVulkan/renderer/PostStage.cpp";
    const auto post_stage_text_opt = readFileText(post_stage_path);
    ASSERT_TRUE(post_stage_text_opt.has_value()) << "missing " << post_stage_path.string();
    const std::string &post_stage_text = *post_stage_text_opt;

    EXPECT_NE(post_stage_text.find("vk::AttachmentLoadOp::eDontCare"), std::string::npos)
      << post_stage_path.string()
      << " no longer declares its colour attachment with vk::AttachmentLoadOp::eDontCare - the sky is in the HDR "
         "target, so post's fullscreen triangle writes every pixel and nothing under it survives";
    EXPECT_EQ(post_stage_text.find("vk::AttachmentLoadOp::eLoad"), std::string::npos)
      << post_stage_path.string() << " loads the swapchain image, which nothing wrote before post";
    EXPECT_EQ(post_stage_text.find("ClearColorValue"), std::string::npos)
      << post_stage_path.string() << " builds a ClearColorValue for a pass that overwrites every pixel";
    // buildRenderingAttachmentInfo defaults the clear value, so an overwriting attachment names none at all.
    EXPECT_EQ(post_stage_text.find("vk::ClearValue"), std::string::npos)
      << post_stage_path.string() << " builds a vk::ClearValue for a pass that overwrites every pixel";
    EXPECT_EQ(post_stage_text.find("setAlphaBlending(true)"), std::string::npos)
      << post_stage_path.string() << " blends over a swapchain image whose contents are undefined";

    const fs::path post_slang_path = slangRoot() / "post" / "post.slang";
    const auto post_slang_text_opt = readFileText(post_slang_path);
    ASSERT_TRUE(post_slang_text_opt.has_value()) << "missing " << post_slang_path.string();
    const std::string &post_slang_text = *post_slang_text_opt;

    std::size_t sample_count = 0;
    for (const std::string needle : { "noisyTxt.Sample", "noisyTxt.Load" }) {
        std::size_t pos = 0;
        while ((pos = post_slang_text.find(needle, pos)) != std::string::npos) {
            ++sample_count;
            pos += needle.size();
        }
    }
    EXPECT_EQ(sample_count, 1U)
      << post_slang_path.string() << " samples noisyTxt " << sample_count
      << " time(s) in fs_main - expected exactly one fetch into a float4, with .rgb/.a taken from it";
}
