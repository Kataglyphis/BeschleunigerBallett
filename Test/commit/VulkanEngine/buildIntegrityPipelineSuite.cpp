// Build integrity: pipelines, attachments and passes go through the shared builders and the stage registry.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "BuildIntegrityScan.hpp"

using namespace Kataglyphis::TestSupport::BuildIntegrity;

// Compute pipelines are built only by createComputePipeline.
TEST(BuildIntegrity, ComputePipelinesAreCreatedThroughTheSharedHelper)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(sourceExists(src_root)) << "missing " << src_root.string();

    static const char *const kRawComputePipelineTypeName = "vk::ComputePipelineCreateInfo";

    // The pure builder and the device-side helper's own implementation.
    static const std::array<const char *, 2> kExemptFiles = {
        "Src/GraphicsEngineVulkan/common/ComputePipelineHelper.hpp",
        "Src/GraphicsEngineVulkan/vulkan_base/ShaderHelper.cpp"
    };

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx") { continue; }

        const std::string relative_file = fs::relative(path, repo_root).generic_string();
        if (std::find(kExemptFiles.begin(), kExemptFiles.end(), relative_file) != kExemptFiles.end()) { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        std::size_t line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            if (line.find(kRawComputePipelineTypeName) == std::string::npos) { continue; }
            violations.push_back(relative_file + ":" + std::to_string(line_number) + ": " + line);
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " hand-rolled vk::ComputePipelineCreateInfo found under Src/ - create compute pipelines through "
         "createComputePipeline (vulkan_base/ShaderHelper.ixx) instead:"
      << joinViolations(violations);
}

// Stages and shader groups go through the shared builders: a wrong pName or UNUSED_KHR sentinel compiles fine.
TEST(BuildIntegrity, EveryPipelineShaderStageGoesThroughTheSharedBuilder)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(sourceExists(src_root)) << "missing " << src_root.string();

    // ShaderStageHelper.hpp is the builders' own definition.
    static const std::array<const char *, 1> kExemptFiles = { "Src/GraphicsEngineVulkan/common/ShaderStageHelper.hpp" };

    static const std::regex kPNameAssignment(R"(\.pName\s*=[^=])");
    static const std::regex kStageAssignment(R"(\.stage\s*=[^=])");
    // Local declarations only: span or vector parameters of the type construct nothing, so no '<' may precede it.
    static const std::regex kLocalDeclaration(R"((?:^|[^<,]\s)vk::PipelineShaderStageCreateInfo\s+\w+\s*[{;])");

    std::size_t checked = 0;
    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx" && path.extension() != ".hpp") { continue; }

        const std::string relative_file = fs::relative(path, repo_root).generic_string();
        if (std::find(kExemptFiles.begin(), kExemptFiles.end(), relative_file) != kExemptFiles.end()) { continue; }

        const auto contentsOpt = readFileText(path);
        if (!contentsOpt.has_value()) { continue; }
        ++checked;

        bool has_pname_assignment = false;
        bool has_local_declaration = false;
        bool has_stage_assignment = false;
        std::istringstream stream(*contentsOpt);
        std::string line;
        while (std::getline(stream, line)) {
            if (std::regex_search(line, kPNameAssignment)) { has_pname_assignment = true; }
            if (std::regex_search(line, kLocalDeclaration)) { has_local_declaration = true; }
            if (std::regex_search(line, kStageAssignment)) { has_stage_assignment = true; }
        }

        if (has_pname_assignment) {
            violations.push_back(relative_file + ": hand-assigns .pName instead of using buildShaderStageCreateInfo");
        }
        if (has_local_declaration && has_stage_assignment) {
            violations.push_back(relative_file
              + ": declares a local vk::PipelineShaderStageCreateInfo and hand-assigns .stage instead of using "
                "buildShaderStageCreateInfo");
        }
    }

    ASSERT_GT(checked, 0u) << "found zero files under " << src_root.string() << " - the scan itself is broken";

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " hand-rolled vk::PipelineShaderStageCreateInfo field assignment(s) found under "
         "Src/GraphicsEngineVulkan/ - build shader stages through common/ShaderStageHelper.hpp's "
         "buildShaderStageCreateInfo instead:"
      << joinViolations(violations);
}

// Pipelines and their layouts are destroyed only through destroyPipelineAndLayout.
TEST(BuildIntegrity, PipelineTeardownGoesThroughTheSharedHelper)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(sourceExists(src_root)) << "missing " << src_root.string();

    static const char *const kRawTeardownCall = "destroyPipelineLayout(";

    // PipelineLayoutHelper.hpp is the helper's own definition.
    static const std::array<const char *, 1> kExemptFiles = {
        "Src/GraphicsEngineVulkan/common/PipelineLayoutHelper.hpp"
    };

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx") { continue; }

        const std::string relative_file = fs::relative(path, repo_root).generic_string();
        if (std::find(kExemptFiles.begin(), kExemptFiles.end(), relative_file) != kExemptFiles.end()) { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        std::size_t line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            if (line.find(kRawTeardownCall) == std::string::npos) { continue; }
            violations.push_back(relative_file + ":" + std::to_string(line_number) + ": " + line);
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " hand-rolled destroyPipelineLayout(...) call(s) found under Src/GraphicsEngineVulkan/ - destroy "
         "pipelines and their layouts through Kataglyphis::destroyPipelineAndLayout "
         "(common/PipelineLayoutHelper.hpp) instead:"
      << joinViolations(violations);
}

// Depth attachments come from createDepthAttachment; the shadow array is exempt by its line marker, not by file.
TEST(BuildIntegrity, NoStageHandRollsTheDepthAttachmentChain)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(sourceExists(src_root)) << "missing " << src_root.string();

    // The image-usage spelling only; the access, layout and format-feature spellings are unrelated.
    static const char *const kUsageBit = "ImageUsageFlagBits::eDepthStencilAttachment";
    static const char *const kMarkerPrefix = "DEPTH_ATTACHMENT_CHAIN_OK: ";

    // The helper's own definition is exempt from its own rule.
    static const char *const kHelperFile = "Src/GraphicsEngineVulkan/renderer/DepthAttachment.ixx";

    std::vector<std::string> violations;
    bool marker_found = false;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx") { continue; }

        const std::string relative_file = fs::relative(path, repo_root).generic_string();
        if (relative_file == kHelperFile) { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        std::size_t line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            if (line.find(kUsageBit) == std::string::npos) { continue; }
            if (line.find(kMarkerPrefix) != std::string::npos) {
                marker_found = true;
                continue;
            }
            violations.push_back(relative_file + ":" + std::to_string(line_number) + ": " + line);
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " hand-rolled vk::ImageUsageFlagBits::eDepthStencilAttachment use(s) found under "
         "Src/GraphicsEngineVulkan/ - route depth attachment creation through "
         "Kataglyphis::VulkanRendererInternals::createDepthAttachment (renderer/DepthAttachment.ixx) instead, "
         "or add a \"// DEPTH_ATTACHMENT_CHAIN_OK: <marker>\" trailing comment on the line if the site is a "
         "deliberate non-goal:"
      << joinViolations(violations);

    EXPECT_TRUE(marker_found) << "expected to find the CascadedShadowMap.cpp shadow-map-array exemption marker "
                                 "(\"// DEPTH_ATTACHMENT_CHAIN_OK: ...\") in source - if it was removed, delete "
                                 "this check too";
}

// Every pass renders through vkCmdBeginRendering and records sync2 barriers; nothing builds a render pass any more.
TEST(BuildIntegrity, NoPassUsesARenderPassFramebufferOrLegacyBarrier)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(sourceExists(src_root)) << "missing " << src_root.string();

    // A render pass or framebuffer brings back the recreateSwapChain rebuild; a sync1 barrier mixes two stage models.
    static const std::array<const char *, 9> kForbidden = { "vk::RenderPass",
        "vk::Framebuffer",
        "beginRenderPass(",
        "nextSubpass(",
        "createRenderPass(",
        "createFramebuffer(",
        "VK_SUBPASS_EXTERNAL",
        ".pipelineBarrier(",
        "vk::ImageMemoryBarrier " };

    std::vector<std::string> violations;
    std::set<std::string> files_beginning_rendering;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx" && path.extension() != ".hpp") { continue; }

        const std::string relative_file = fs::relative(path, repo_root).generic_string();
        const auto lines_opt = readFileLines(path);
        if (!lines_opt) { continue; }
        std::size_t line_number = 0;
        for (const auto &line : *lines_opt) {
            ++line_number;
            if (line.find("beginRendering(") != std::string::npos) { files_beginning_rendering.insert(relative_file); }
            for (const char *forbidden : kForbidden) {
                if (line.find(forbidden) == std::string::npos) { continue; }
                violations.push_back(relative_file + ":" + std::to_string(line_number) + ": " + line);
                break;
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " render pass, framebuffer or sync1 barrier use(s) found under Src/GraphicsEngineVulkan/ - begin passes "
         "with vkCmdBeginRendering (common/RenderingHelper.hpp) and record barriers through "
         "Kataglyphis::recordImageBarriers / vkCmdPipelineBarrier2 (common/ImageBarrierHelper.hpp):"
      << joinViolations(violations);

    // Control: the scan must see the passes it guards, or deleting every pass would pass it.
    static const std::array<const char *, 6> kPassFiles = { "Src/GraphicsEngineVulkan/renderer/Rasterizer.cpp",
        "Src/GraphicsEngineVulkan/renderer/DeferredRasterizer.cpp",
        "Src/GraphicsEngineVulkan/renderer/PostStage.cpp",
        "Src/GraphicsEngineVulkan/renderer/FxaaStage.cpp",
        "Src/GraphicsEngineVulkan/scene/sky_box/SkyBox.cpp",
        "Src/GraphicsEngineVulkan/scene/light/directional_light/CascadedShadowMap.cpp" };
    for (const char *pass_file : kPassFiles) {
        EXPECT_TRUE(files_beginning_rendering.contains(pass_file))
          << pass_file << " no longer calls beginRendering( - the scan above no longer sees that pass";
    }
}

// Post, FXAA and the traced sky test nothing against depth, so none may render with one; the raster sky is a draw.
TEST(BuildIntegrity, PostAndSkyboxPassesRenderWithoutDepth)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    for (const char *relative_path : { "Src/GraphicsEngineVulkan/renderer/PostStage.cpp",
           "Src/GraphicsEngineVulkan/renderer/FxaaStage.cpp",
           "Src/GraphicsEngineVulkan/scene/sky_box/SkyBox.cpp" }) {
        const auto contents_opt = readFileText(repo_root / relative_path);
        ASSERT_TRUE(contents_opt.has_value()) << "missing " << relative_path;
        const std::string &contents = *contents_opt;

        EXPECT_EQ(contents.find("eDepthStencilAttachment"), std::string::npos)
          << relative_path << " references a depth-stencil attachment layout or access - this pass has no depth";
        EXPECT_EQ(contents.find("depthBufferImage"), std::string::npos)
          << relative_path << " references a depth buffer - this pass has no depth";
        EXPECT_NE(contents.find("std::span<const vk::RenderingAttachmentInfo>(&color_attachment, 1), nullptr)"),
          std::string::npos)
          << relative_path << " no longer begins rendering with one colour attachment and a null depth attachment";
    }

    // Control: the rasterizers own real depth buffers and must keep rendering into them.
    for (const char *relative_path : { "Src/GraphicsEngineVulkan/renderer/Rasterizer.cpp",
           "Src/GraphicsEngineVulkan/renderer/DeferredRasterizer.cpp" }) {
        const auto contents_opt = readFileText(repo_root / relative_path);
        ASSERT_TRUE(contents_opt.has_value()) << "missing " << relative_path;
        EXPECT_NE(contents_opt->find("&depth_attachment"), std::string::npos)
          << relative_path << " no longer renders with a depth attachment - this gate must not pass by deleting "
          << "depth everywhere";
    }
}

// Plain colour attachments come from createColorAttachment; other views are exempt only by their line marker.
TEST(BuildIntegrity, NoStageHandRollsTheColorAttachmentChain)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(sourceExists(src_root)) << "missing " << src_root.string();

    static const char *const kViewCall = "createImageView(";
    static const char *const kColorAspect = "ImageAspectFlagBits::eColor";
    static const char *const kMarkerPrefix = "COLOR_ATTACHMENT_CHAIN_OK: ";

    // The helper's own definition is exempt from its own rule.
    static const char *const kHelperFile = "Src/GraphicsEngineVulkan/renderer/ColorAttachment.ixx";

    std::vector<std::string> violations;
    bool marker_found = false;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx") { continue; }

        const std::string relative_file = fs::relative(path, repo_root).generic_string();
        if (relative_file == kHelperFile) { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        std::size_t line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            if (line.find(kViewCall) == std::string::npos) { continue; }
            if (line.find(kColorAspect) == std::string::npos) { continue; }
            if (line.find(kMarkerPrefix) != std::string::npos) {
                marker_found = true;
                continue;
            }
            violations.push_back(relative_file + ":" + std::to_string(line_number) + ": " + line);
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " hand-rolled createImageView(..., vk::ImageAspectFlagBits::eColor, ...) call(s) found under "
         "Src/GraphicsEngineVulkan/ - route plain colour attachment creation through "
         "Kataglyphis::VulkanRendererInternals::createColorAttachment (renderer/ColorAttachment.ixx) instead, "
         "or add a \"// COLOR_ATTACHMENT_CHAIN_OK: <marker>\" trailing comment on the line if the site is a "
         "deliberate non-goal:"
      << joinViolations(violations);

    EXPECT_TRUE(marker_found) << "expected to find at least one of the non-goal exemption markers "
                                 "(\"// COLOR_ATTACHMENT_CHAIN_OK: ...\") in source - if all of them were "
                                 "removed, delete this check too";
}

// Each depth-owning stage assigns depth_format once, so the render-pass format and depth image cannot diverge.
TEST(BuildIntegrity, EveryRenderStageDerivesItsDepthFormatOnce)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const std::array<const char *, 3> kFiles = {
        "Src/GraphicsEngineVulkan/renderer/Rasterizer.cpp",
        "Src/GraphicsEngineVulkan/renderer/DeferredRasterizer.cpp",
        "Src/GraphicsEngineVulkan/scene/light/directional_light/CascadedShadowMap.cpp",
    };

    static const char *const kAssignment = "depth_format =";

    for (const char *relative_path : kFiles) {
        const fs::path path = repo_root / relative_path;
        ASSERT_TRUE(sourceExists(path)) << "missing " << path.string();

        const auto contentsOpt = readFileText(path);
        ASSERT_TRUE(contentsOpt.has_value()) << "could not open " << path.string();
        const std::string &contents = *contentsOpt;

        std::size_t count = 0;
        std::size_t pos = 0;
        while ((pos = contents.find(kAssignment, pos)) != std::string::npos) {
            ++count;
            pos += std::strlen(kAssignment);
        }

        EXPECT_EQ(count, 1U) << relative_path << " assigns depth_format " << count
                             << " time(s); it must be derived exactly once and cached in a member, so the "
                                "render-pass attachment and the image it is paired with cannot diverge";
    }
}

// A pass recorded straight from record_commands skips the registry's order, timestamps, resize and reload hooks.
TEST(BuildIntegrity, RecordCommandsRecordsPassesOnlyThroughTheStageRegistry)
{
    const fs::path renderer_path = repoRoot() / "Src" / "GraphicsEngineVulkan" / "renderer" / "VulkanRenderer.cpp";
    const auto text = readFileText(renderer_path);
    ASSERT_TRUE(text.has_value()) << "could not open " << renderer_path.string();

    const auto span = function_body_span(*text, "Kataglyphis::VulkanRenderer::record_commands");
    ASSERT_TRUE(span.has_value()) << "VulkanRenderer::record_commands was not found in " << renderer_path.string();
    const std::string body = text->substr(span->first, span->second - span->first);

    EXPECT_NE(body.find("renderStages.recordFrame("), std::string::npos)
      << "record_commands no longer records the frame through renderStages.recordFrame";
    for (const char *direct : { "recordCommands(",
           "recordComputeCommands(",
           "recordUpsampleCommands(",
           "recordRasterPass(",
           "recordRaytracingOrPathTracing(",
           "recordSkippedPass(" }) {
        EXPECT_EQ(body.find(direct), std::string::npos)
          << "record_commands calls " << direct << " directly; register the pass as a RenderStage instead";
    }
}

// Both raster passes bind descriptorSets[0]; an empty span there is undefined behaviour unless refused first.
TEST(BuildIntegrity, RasterPassesRefuseAnEmptyDescriptorSpan)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const std::array<std::pair<const char *, const char *>, 2> kPasses = { {
      { "Src/GraphicsEngineVulkan/renderer/Rasterizer.cpp", "Rasterizer::recordCommands(" },
      { "Src/GraphicsEngineVulkan/renderer/DeferredRasterizer.cpp", "DeferredRasterizer::recordCommands(" },
    } };
    for (const auto &[relative, function] : kPasses) {
        const auto text = readFileText(repo_root / relative);
        ASSERT_TRUE(text.has_value()) << "could not open " << relative;
        const std::string body = extract_function_body(*text, function);
        ASSERT_FALSE(body.empty()) << "could not locate " << function << " in " << relative;
        const std::size_t guard = body.find("descriptorSets.empty()");
        const std::size_t first_bind = body.find("bindDescriptorSets(");
        ASSERT_NE(first_bind, std::string::npos) << function << " binds no descriptor set any more";
        EXPECT_LT(guard, first_bind) << function << " binds descriptorSets before refusing an empty span, as "
                                     << "Clouds::recordComputeCommands does";
    }
}

// cleanUp() and recreateFrameResources() share releaseFrameTextures(), so their teardowns cannot drift apart.
TEST(BuildIntegrity, RasterStagesReleaseFrameTexturesThroughOneHelper)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    struct Target
    {
        fs::path path;
        std::string clean_up_name;
        std::string recreate_name;
    };

    const std::array<Target, 2> targets{ {
      { repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "Rasterizer.cpp",
        "Rasterizer::cleanUp",
        "Rasterizer::recreateFrameResources" },
      { repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "DeferredRasterizer.cpp",
        "DeferredRasterizer::cleanUp",
        "DeferredRasterizer::recreateFrameResources" },
    } };

    static const std::array<const char *, 3> kTextureCleanUpTokens{
        "tex->cleanUp()",
        "texture->cleanUp()",
        "depthBufferImage->cleanUp()",
    };

    for (const auto &target : targets) {
        const auto text_opt = readFileText(target.path);
        ASSERT_TRUE(text_opt.has_value()) << "could not read " << target.path.string();
        const std::string &text = *text_opt;

        for (const auto &function_name : { target.clean_up_name, target.recreate_name }) {
            const auto span = function_body_span(text, function_name);
            ASSERT_TRUE(span.has_value())
              << target.path.string() << " is missing a body for " << function_name << "(...)";
            const std::string body = text.substr(span->first, span->second - span->first);

            EXPECT_NE(body.find("releaseFrameTextures()"), std::string::npos)
              << target.path.string() << "'s " << function_name
              << " no longer calls releaseFrameTextures() - the frame-texture teardown must go through the "
                 "shared helper rather than being pasted back in inline.";

            for (const char *token : kTextureCleanUpTokens) {
                EXPECT_EQ(body.find(token), std::string::npos)
                  << target.path.string() << "'s " << function_name << " contains an inline \"" << token
                  << "\" - frame-texture teardown must go through releaseFrameTextures() instead of a "
                     "second hand-written copy.";
            }
        }
    }
}
