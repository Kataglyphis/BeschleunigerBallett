// Build integrity: shader hot reload is in every build, reaches every SPIR-V loader and rebuilds what it must.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <regex>
#include <string>
#include <vector>

#include "BuildIntegrityScan.hpp"

using namespace Kataglyphis::TestSupport::BuildIntegrity;

// F5 reloads the shaders in every native build (owner 2026-11-19), so no request site may sit in an NDEBUG block.
TEST(BuildIntegrity, ShaderReloadRequestsAreInEveryBuild)
{
    const fs::path gui_path = repoRoot() / "Src" / "GraphicsEngineVulkan" / "gui" / "GUI.cpp";
    const auto lines = readFileLines(gui_path);
    ASSERT_TRUE(lines.has_value()) << "could not open " << gui_path.string();

    // Each open #if block's directive, its #else and #elif appended: the #else of an NDEBUG test depends on it too.
    std::vector<std::string> open_blocks;
    int sites = 0;
    std::vector<std::string> gated;
    for (std::size_t i = 0; i < lines->size(); ++i) {
        const std::string line = strip_line_comment((*lines)[i]);
        const auto first = line.find_first_not_of(" \t");
        const std::string directive = first == std::string::npos ? std::string{} : line.substr(first);
        if (directive.starts_with("#if")) {
            open_blocks.push_back(directive);
        } else if (directive.starts_with("#el") && !open_blocks.empty()) {
            open_blocks.back() += " " + directive;
        } else if (directive.starts_with("#endif") && !open_blocks.empty()) {
            open_blocks.pop_back();
        }
        if (line.find("shader_hot_reload_triggered = true") == std::string::npos) { continue; }
        ++sites;
        if (std::any_of(open_blocks.begin(), open_blocks.end(), [](const std::string &block) {
                return block.find("NDEBUG") != std::string::npos;
            })) {
            gated.push_back("GUI.cpp:" + std::to_string(i + 1));
        }
    }
    EXPECT_GE(sites, 3) << "the F5 key, the File menu and the Renderer tab's button each raise the reload request";
    EXPECT_TRUE(gated.empty()) << "shader reload requests a Release build compiles out:" << joinViolations(gated);
}

// The renderer must call every stage's shaderHotReload, or reload silently skips it; counted, not hand-mapped.
TEST(BuildIntegrity, EveryShaderHotReloadImplementationIsCalledByTheRenderer)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(sourceExists(src_root)) << "missing " << src_root.string();

    static const std::regex kDefinition(R"(([A-Za-z_][A-Za-z0-9_]*)::shaderHotReload\s*\()");
    static const char *const kExcludedClass = "VulkanRenderer";

    std::vector<std::string> implementing_classes;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp") { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        for (const auto &line : *lines) {
            std::smatch match;
            if (std::regex_search(line, match, kDefinition)) {
                std::string class_name = match[1].str();
                if (class_name != kExcludedClass) { implementing_classes.push_back(class_name); }
            }
        }
    }

    ASSERT_GT(implementing_classes.size(), 0u)
      << "found zero <Stage>::shaderHotReload(...) implementations under " << src_root.string()
      << " - the scan itself is broken, or every implementation was removed and this gate is now vacuous";

    const fs::path renderer_path = repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "VulkanRenderer.cpp";
    const auto renderer_contents_opt = readFileText(renderer_path);
    ASSERT_TRUE(renderer_contents_opt.has_value()) << "missing " << renderer_path.string();
    const std::string &renderer_contents = *renderer_contents_opt;

    // The reload runs every stage's hook, and the built-in stages' hooks are set where they are registered.
    const auto reload_span = function_body_span(renderer_contents, "Kataglyphis::VulkanRenderer::shaderHotReload");
    ASSERT_TRUE(reload_span.has_value()) << "VulkanRenderer::shaderHotReload is no longer defined in "
                                         << renderer_path.string();
    EXPECT_NE(renderer_contents.substr(reload_span->first, reload_span->second - reload_span->first)
                .find("renderStages.shaderHotReload()"),
      std::string::npos)
      << "VulkanRenderer::shaderHotReload no longer runs the stage registry's reload hooks";

    const std::size_t signature_pos = renderer_contents.find("VulkanRenderer::registerBuiltInStages(");
    ASSERT_NE(signature_pos, std::string::npos)
      << "VulkanRenderer::registerBuiltInStages is no longer defined in " << renderer_path.string();

    const std::size_t body_open = renderer_contents.find('{', signature_pos);
    ASSERT_NE(body_open, std::string::npos)
      << "could not locate the opening brace of VulkanRenderer::registerBuiltInStages";

    int brace_depth = 0;
    std::size_t body_close = std::string::npos;
    for (std::size_t i = body_open; i < renderer_contents.size(); ++i) {
        if (renderer_contents[i] == '{') {
            ++brace_depth;
        } else if (renderer_contents[i] == '}') {
            --brace_depth;
            if (brace_depth == 0) {
                body_close = i;
                break;
            }
        }
    }
    ASSERT_NE(body_close, std::string::npos)
      << "could not brace-match the closing '}' of VulkanRenderer::registerBuiltInStages";

    const std::string body = renderer_contents.substr(body_open, body_close - body_open + 1);
    std::size_t call_sites = 0;
    std::size_t pos = 0;
    while ((pos = body.find(".shaderHotReload(", pos)) != std::string::npos) {
        ++call_sites;
        pos += std::string(".shaderHotReload(").size();
    }

    EXPECT_GE(call_sites, implementing_classes.size())
      << "VulkanRenderer::registerBuiltInStages' reload hooks call only " << call_sites
      << " stage(s)' shaderHotReload, but " << implementing_classes.size()
      << " stage(s) implement it - some implementation is silently unreachable. "
         "Stage classes implementing shaderHotReload: "
      << joinViolations(implementing_classes);
}

// Every SPIR-V loading subsystem needs shaderHotReload; found by its SPIR-V directory, not a hand-kept file list.
TEST(BuildIntegrity, EverySpirvLoadingSubsystemImplementsShaderHotReload)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(sourceExists(src_root)) << "missing " << src_root.string();

    static const char *const kSpirvMarker = "Resources/ShadersSlang/build/spirv/";

    std::vector<fs::path> spirv_loading_files;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || path.extension() != ".cpp") { continue; }

        const auto contents = readFileText(path);
        if (!contents) { continue; }
        if (contents->find(kSpirvMarker) != std::string::npos) { spirv_loading_files.push_back(path); }
    }

    ASSERT_GT(spirv_loading_files.size(), 0u) << "found zero .cpp files referencing " << kSpirvMarker << " under "
                                              << src_root.string() << " - the scan itself is broken";

    std::vector<std::string> missing_implementation;
    for (const fs::path &path : spirv_loading_files) {
        const auto contents = readFileText(path);
        ASSERT_TRUE(contents.has_value()) << "could not re-open " << path.string();

        bool declares_reload = contents->find("shaderHotReload") != std::string::npos;
        if (!declares_reload) {
            const fs::path paired_ixx = fs::path(path).replace_extension(".ixx");
            if (const auto ixx_contents = readFileText(paired_ixx)) {
                declares_reload = ixx_contents->find("shaderHotReload") != std::string::npos;
            }
        }

        if (!declares_reload) { missing_implementation.push_back(fs::relative(path, repo_root).string()); }
    }

    EXPECT_TRUE(missing_implementation.empty())
      << missing_implementation.size()
      << " subsystem(s) load SPIR-V but declare no shaderHotReload (neither the .cpp nor its paired .ixx): "
      << joinViolations(missing_implementation);

    const fs::path renderer_path = repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "VulkanRenderer.cpp";
    const auto renderer_contents_opt = readFileText(renderer_path);
    ASSERT_TRUE(renderer_contents_opt.has_value()) << "missing " << renderer_path.string();
    const std::string &renderer_contents = *renderer_contents_opt;

    // The reload runs every stage's hook, and the built-in stages' hooks are set where they are registered.
    const auto reload_span = function_body_span(renderer_contents, "Kataglyphis::VulkanRenderer::shaderHotReload");
    ASSERT_TRUE(reload_span.has_value()) << "VulkanRenderer::shaderHotReload is no longer defined in "
                                         << renderer_path.string();
    EXPECT_NE(renderer_contents.substr(reload_span->first, reload_span->second - reload_span->first)
                .find("renderStages.shaderHotReload()"),
      std::string::npos)
      << "VulkanRenderer::shaderHotReload no longer runs the stage registry's reload hooks";

    const std::size_t signature_pos = renderer_contents.find("VulkanRenderer::registerBuiltInStages(");
    ASSERT_NE(signature_pos, std::string::npos)
      << "VulkanRenderer::registerBuiltInStages is no longer defined in " << renderer_path.string();

    const std::size_t body_open = renderer_contents.find('{', signature_pos);
    ASSERT_NE(body_open, std::string::npos)
      << "could not locate the opening brace of VulkanRenderer::registerBuiltInStages";

    int brace_depth = 0;
    std::size_t body_close = std::string::npos;
    for (std::size_t i = body_open; i < renderer_contents.size(); ++i) {
        if (renderer_contents[i] == '{') {
            ++brace_depth;
        } else if (renderer_contents[i] == '}') {
            --brace_depth;
            if (brace_depth == 0) {
                body_close = i;
                break;
            }
        }
    }
    ASSERT_NE(body_close, std::string::npos)
      << "could not brace-match the closing '}' of VulkanRenderer::registerBuiltInStages";

    const std::string body = renderer_contents.substr(body_open, body_close - body_open + 1);
    std::size_t call_sites = 0;
    std::size_t pos = 0;
    while ((pos = body.find(".shaderHotReload(", pos)) != std::string::npos) {
        ++call_sites;
        pos += std::string(".shaderHotReload(").size();
    }

    EXPECT_GE(call_sites, spirv_loading_files.size())
      << "VulkanRenderer::registerBuiltInStages' reload hooks call only " << call_sites
      << " stage(s)' shaderHotReload, but " << spirv_loading_files.size()
      << " subsystem(s) load SPIR-V - some reload is silently unreachable.";
}

// Hot reload must destroy the old pipeline layout before recreating it, or each reload leaks one.
TEST(BuildIntegrity, EveryShaderHotReloadDestroysThePipelineLayoutItRecreates)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(sourceExists(src_root)) << "missing " << src_root.string();

    static const std::regex kDefinition(R"(([A-Za-z_][A-Za-z0-9_]*)::shaderHotReload\s*\()");
    static const char *const kExcludedClass = "VulkanRenderer";// delegates to the per-stage implementations

    std::vector<std::string> offenders;
    std::size_t checked = 0;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || path.extension() != ".cpp") { continue; }

        const auto contentsOpt = readFileText(path);
        if (!contentsOpt) { continue; }
        const std::string &contents = *contentsOpt;

        std::smatch match;
        std::string remaining = contents;
        std::size_t offset = 0;
        while (std::regex_search(remaining, match, kDefinition)) {
            const std::size_t signature_pos = offset + static_cast<std::size_t>(match.position(0));
            const std::string class_name = match[1].str();

            const std::size_t body_open = contents.find('{', signature_pos);
            if (body_open == std::string::npos) { break; }

            int brace_depth = 0;
            std::size_t body_close = std::string::npos;
            for (std::size_t i = body_open; i < contents.size(); ++i) {
                if (contents[i] == '{') {
                    ++brace_depth;
                } else if (contents[i] == '}') {
                    --brace_depth;
                    if (brace_depth == 0) {
                        body_close = i;
                        break;
                    }
                }
            }
            ASSERT_NE(body_close, std::string::npos)
              << "could not brace-match the closing '}' of " << class_name << "::shaderHotReload in " << path.string();

            if (class_name != kExcludedClass) {
                ++checked;
                const std::string body = contents.substr(body_open, body_close - body_open + 1);
                if (body.find("destroyPipelineLayout(") == std::string::npos
                    && body.find("destroyPipelineAndLayout(") == std::string::npos) {
                    offenders.push_back(class_name + " (" + fs::relative(path, repo_root).string() + ")");
                }
            }

            const std::size_t advance = body_close + 1;
            offset = advance;
            remaining = contents.substr(advance);
        }
    }

    ASSERT_GT(checked, 0u) << "found zero non-delegating <Stage>::shaderHotReload(...) implementations under "
                           << src_root.string() << " - the scan itself is broken";

    EXPECT_TRUE(offenders.empty())
      << offenders.size()
      << " shaderHotReload(...) implementation(s) recreate a pipeline layout without destroying the previous "
         "one first, leaking a vk::PipelineLayout on every hot reload: "
      << joinViolations(offenders);
}

// Shader group handles are valid only for their own pipeline, so a hot reload must rebuild the SBT too.
TEST(BuildIntegrity, RaytracingShaderHotReloadRebuildsTheShaderBindingTable)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path raytracing_path = repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "Raytracing.cpp";
    const auto contentsOpt = readFileText(raytracing_path);
    ASSERT_TRUE(contentsOpt.has_value()) << "missing " << raytracing_path.string();
    const std::string &contents = *contentsOpt;

    const std::size_t signature_pos = contents.find("Raytracing::shaderHotReload(");
    ASSERT_NE(signature_pos, std::string::npos)
      << "Raytracing::shaderHotReload is no longer defined in " << raytracing_path.string();

    const std::size_t body_open = contents.find('{', signature_pos);
    ASSERT_NE(body_open, std::string::npos) << "could not locate the opening brace of Raytracing::shaderHotReload";

    int brace_depth = 0;
    std::size_t body_close = std::string::npos;
    for (std::size_t i = body_open; i < contents.size(); ++i) {
        if (contents[i] == '{') {
            ++brace_depth;
        } else if (contents[i] == '}') {
            --brace_depth;
            if (brace_depth == 0) {
                body_close = i;
                break;
            }
        }
    }
    ASSERT_NE(body_close, std::string::npos) << "could not brace-match the closing '}' of Raytracing::shaderHotReload";

    const std::string body = contents.substr(body_open, body_close - body_open + 1);
    EXPECT_TRUE(body.find("recreateSBT") != std::string::npos || body.find("createSBT") != std::string::npos)
      << "Raytracing::shaderHotReload no longer rebuilds the shader binding table after recreating the "
         "pipeline - shader group handles are only valid for the pipeline that produced them, so every "
         "hot reload must call recreateSBT() (or createSBT()) after createGraphicsPipeline(), or "
         "traceRaysKHR reads shader-group handles from a destroyed pipeline";
}
