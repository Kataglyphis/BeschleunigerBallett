// Build integrity: resources release before they recreate; descriptors, features and AS rebuilds use helpers.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "BuildIntegrityScan.hpp"

using namespace Kataglyphis::TestSupport::BuildIntegrity;

namespace {

// First non-comment statement in the body of `qualified_name`; assumes its parameter list holds no parentheses.
std::optional<std::string> first_statement_of_function(const std::string &text, const std::string &qualified_name)
{
    const std::string signature = qualified_name + "(";
    const std::size_t sig_pos = text.find(signature);
    if (sig_pos == std::string::npos) { return std::nullopt; }

    std::size_t pos = sig_pos + signature.size();
    int depth = 1;
    while (pos < text.size() && depth > 0) {
        if (text[pos] == '(') {
            ++depth;
        } else if (text[pos] == ')') {
            --depth;
        }
        ++pos;
    }
    if (depth != 0) { return std::nullopt; }

    const std::size_t brace_pos = text.find('{', pos);
    if (brace_pos == std::string::npos) { return std::nullopt; }

    std::size_t body_pos = brace_pos + 1;
    while (body_pos < text.size()) {
        while (body_pos < text.size() && std::isspace(static_cast<unsigned char>(text[body_pos])) != 0) { ++body_pos; }
        if (body_pos + 1 < text.size() && text[body_pos] == '/' && text[body_pos + 1] == '/') {
            const std::size_t newline = text.find('\n', body_pos);
            body_pos = newline == std::string::npos ? text.size() : newline + 1;
            continue;
        }
        break;
    }

    const std::size_t stmt_end = text.find(';', body_pos);
    if (stmt_end == std::string::npos) { return std::nullopt; }
    return text.substr(body_pos, stmt_end - body_pos + 1);
}

}// namespace

// create() must release first, or a second call leaks the old allocation; a text check, as behaviour needs a device.
TEST(BuildIntegrity, ResourceCreateReleasesThePreviousAllocation)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path vulkan_base_dir = repo_root / "Src" / "GraphicsEngineVulkan" / "vulkan_base";
    const fs::path scene_dir = repo_root / "Src" / "GraphicsEngineVulkan" / "scene";

    struct Target
    {
        fs::path source;
        std::string qualified_name;
        std::string expected_first_statement;
    };
    const std::array<Target, 4> targets = {
        Target{ vulkan_base_dir / "VulkanBuffer.cpp", "Kataglyphis::VulkanBuffer::create", "cleanUp();" },
        Target{ vulkan_base_dir / "VulkanImage.cpp", "Kataglyphis::VulkanImage::create", "cleanUp();" },
        Target{ vulkan_base_dir / "VulkanImageView.cpp", "Kataglyphis::VulkanImageView::create", "cleanUp();" },
        Target{ scene_dir / "Texture.cpp", "Kataglyphis::Texture::createTextureSampler", "releaseSampler();" },
    };

    for (const auto &target : targets) {
        const auto text = readFileText(target.source);
        ASSERT_TRUE(text.has_value()) << "could not read " << target.source.string();

        const auto first_statement = first_statement_of_function(*text, target.qualified_name);
        ASSERT_TRUE(first_statement.has_value())
          << target.qualified_name << "(...) definition not found in " << target.source.string();

        EXPECT_EQ(*first_statement, target.expected_first_statement)
          << target.qualified_name << "'s first statement must be " << target.expected_first_statement
          << ", so calling create() on an already-created instance "
             "releases the previous allocation instead of leaking it. Found: \""
          << *first_statement << "\" in " << target.source.string();
    }
}

// Likewise create() must open with releaseGpuResources(), or a second call leaks the layout and pool.
TEST(BuildIntegrity, DescriptorSetGroupCreateReleasesThePreviousAllocation)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path source = repo_root / "Src" / "GraphicsEngineVulkan" / "vulkan_base" / "DescriptorSetGroup.cpp";
    const auto text = readFileText(source);
    ASSERT_TRUE(text.has_value()) << "could not read " << source.string();

    const auto first_statement = first_statement_of_function(*text, "Kataglyphis::DescriptorSetGroup::create");
    ASSERT_TRUE(first_statement.has_value())
      << "Kataglyphis::DescriptorSetGroup::create(...) definition not found in " << source.string();

    EXPECT_EQ(*first_statement, "releaseGpuResources();")
      << "DescriptorSetGroup::create's first statement must be releaseGpuResources(), so calling create() on an "
         "already-created instance releases the previous layout/pool instead of leaking them. Found: \""
      << *first_statement << "\" in " << source.string();
}

// VUID-vkDestroyImage-image-01000: the view release must precede the createImage() that frees the old image.
TEST(BuildIntegrity, ViewIsReleasedBeforeItsImageOnRecreate)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path scene_dir = repo_root / "Src" / "GraphicsEngineVulkan" / "scene";

    struct Target
    {
        fs::path source;
        std::string release_needle;
        std::string create_needle;
    };
    const std::array<Target, 2> targets = {
        Target{ scene_dir / "Texture.cpp", "vulkanImageView.cleanUp();", "createImage(in_device," },
        Target{ scene_dir / "sky_box" / "SkyBox.cpp",
          "cubeMapTexture->releaseImageView();",
          "cubeMapTexture->createImage(device," },
    };

    for (const auto &target : targets) {
        const auto text = readFileText(target.source);
        ASSERT_TRUE(text.has_value()) << "could not read " << target.source.string();

        const std::size_t release_pos = text->find(target.release_needle);
        const std::size_t create_pos = text->find(target.create_needle);
        ASSERT_NE(release_pos, std::string::npos)
          << "could not find \"" << target.release_needle << "\" in " << target.source.string();
        ASSERT_NE(create_pos, std::string::npos)
          << "could not find \"" << target.create_needle << "\" in " << target.source.string();

        EXPECT_LT(release_pos, create_pos)
          << target.source.string() << " must release the previous view (\"" << target.release_needle
          << "\") before it recreates the image (\"" << target.create_needle
          << "\"), or the view outlives the image it was created from.";
    }
}

TEST(BuildIntegrity, EngineSourcesDoNotLogRawVulkanHandles)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(sourceExists(src_root)) << "missing " << src_root.string();

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        const auto extension = path.extension();
        if (extension != ".cpp" && extension != ".ixx") { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        std::size_t line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            if (line.find("spdlog::") == std::string::npos) { continue; }
            const bool has_vk_cast_to_uint64 = line.find("(uint64_t)(Vk") != std::string::npos;
            const bool has_hex_format_with_vk_cast =
              line.find("0x{:x}") != std::string::npos && line.find("Vk") != std::string::npos;
            if (has_vk_cast_to_uint64 || has_hex_format_with_vk_cast) {
                violations.push_back(
                  fs::relative(path, repo_root).generic_string() + ":" + std::to_string(line_number) + ": " + line);
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " spdlog call(s) under Src/ log a raw Vulkan handle - these are noise (the validation layers and Vulkan "
         "debug labels already give a better diagnostic) and read as intentional instrumentation; delete them "
         "instead of demoting to spdlog::debug:"
      << joinViolations(violations);
}

// Descriptor triads go through DescriptorSetGroup; the allowlist is anchored on file paths, not line numbers.
TEST(BuildIntegrity, DescriptorSetsAreCreatedThroughDescriptorSetGroup)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(sourceExists(src_root)) << "missing " << src_root.string();

    static const std::array<const char *, 3> kRawDescriptorTypeNames = {
        "vk::DescriptorSetLayoutCreateInfo", "vk::DescriptorPoolCreateInfo", "vk::DescriptorSetAllocateInfo"
    };

    // The class's own file is exempt outright; GUI.cpp's one ImGui pool gets a budget, so a second triad still fails.
    static const std::array<const char *, 1> kExemptFiles = {
        "Src/GraphicsEngineVulkan/vulkan_base/DescriptorSetGroup.cpp"
    };
    static const std::map<std::string, std::size_t> kDescriptorBudgets = {
        { "Src/GraphicsEngineVulkan/gui/GUI.cpp", 1 },
    };

    std::map<std::string, std::size_t> counts;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp") { continue; }

        const std::string relative_file = fs::relative(path, repo_root).generic_string();
        if (std::find(kExemptFiles.begin(), kExemptFiles.end(), relative_file) != kExemptFiles.end()) { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        for (const auto &line : *lines) {
            for (const char *type_name : kRawDescriptorTypeNames) {
                if (line.find(type_name) == std::string::npos) { continue; }
                ++counts[relative_file];
            }
        }
    }

    std::vector<std::string> violations;
    for (const auto &[file, count] : counts) {
        const auto budget_it = kDescriptorBudgets.find(file);
        const std::size_t budget = budget_it == kDescriptorBudgets.end() ? 0 : budget_it->second;
        if (count == budget) { continue; }
        if (count > budget) {
            violations.push_back(file + ": found " + std::to_string(count)
                                  + " hand-rolled descriptor set layout/pool/set triad(s), budget is "
                                  + std::to_string(budget)
                                  + " - declare bindings through DescriptorSetGroup "
                                    "(vulkan_base/DescriptorSetGroup.ixx) via addBinding()/create() instead");
        } else {
            violations.push_back(file + ": found " + std::to_string(count)
                                 + " hand-rolled descriptor set layout/pool/set triad(s), budget is "
                                 + std::to_string(budget) + " - lower the budget in this test");
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " budget mismatch(es) for hand-rolled descriptor set layout/pool/set triad(s) found under Src/:"
      << joinViolations(violations);
}

TEST(BuildIntegrity, DescriptorBudgetsNameOnlyFilesThatStillHaveTriads)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const std::array<const char *, 3> kRawDescriptorTypeNames = {
        "vk::DescriptorSetLayoutCreateInfo", "vk::DescriptorPoolCreateInfo", "vk::DescriptorSetAllocateInfo"
    };
    static const std::map<std::string, std::size_t> kDescriptorBudgets = {
        { "Src/GraphicsEngineVulkan/gui/GUI.cpp", 1 },
    };

    for (const auto &[file, budget] : kDescriptorBudgets) {
        EXPECT_GT(budget, 0u) << file << ": a budgeted file must have a non-zero budget, or it is a dead entry";

        const fs::path path = repo_root / file;
        ASSERT_TRUE(sourceExists(path)) << "budgeted file no longer exists: " << file;

        const auto lines = readFileLines(path);
        ASSERT_TRUE(lines.has_value()) << "could not read " << file;
        std::size_t count = 0;
        for (const auto &line : *lines) {
            for (const char *type_name : kRawDescriptorTypeNames) {
                if (line.find(type_name) != std::string::npos) { ++count; }
            }
        }
        EXPECT_EQ(count, budget) << file << ": budget says " << budget << " but the file currently has " << count;
    }
}

// AS rebuilds must use refreshAfterSceneChange, or descriptors keep a destroyed TLAS (VUID-vkCmdDispatch-None-08114).
TEST(BuildIntegrity, AccelerationStructureRebuildsGoThroughTheSceneChangeHelper)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(sourceExists(src_root)) << "missing " << src_root.string();

    static const std::array<const char *, 2> kGuardedCalls = { "asManager.createASForScene(", "asManager.createTLAS(" };
    static const char *const kAllowedFile = "Src/GraphicsEngineVulkan/renderer/VulkanRenderer.cpp";
    static const char *const kAllowedFunction = "VulkanRenderer::refreshAfterSceneChange";
    static const std::regex kFunctionSignature(R"(([A-Za-z_][A-Za-z0-9_:]*::[A-Za-z_][A-Za-z0-9_]*)\s*\()");

    std::vector<std::string> violations;
    std::size_t matches_found = 0;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        const auto extension = path.extension();
        if (extension != ".cpp" && extension != ".ixx") { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        const std::string relative_file = fs::relative(path, repo_root).generic_string();

        std::size_t line_number = 0;
        int brace_depth = 0;
        std::string current_function;
        bool awaiting_open_brace = false;
        std::string pending_function_name;

        for (const auto &line : *lines) {
            ++line_number;

            if (awaiting_open_brace) {
                if (line.find('{') != std::string::npos) {
                    current_function = pending_function_name;
                    awaiting_open_brace = false;
                } else if (line.find(';') != std::string::npos) {
                    // A declaration/prototype after all, not a definition.
                    awaiting_open_brace = false;
                    pending_function_name.clear();
                }
            } else if (brace_depth == 0 && line.find("::") != std::string::npos) {
                std::smatch match;
                if (std::regex_search(line, match, kFunctionSignature)) {
                    if (line.find('{') != std::string::npos) {
                        // Signature and brace share this line; the brace counting below clears it when the body closes.
                        current_function = match[1].str();
                    } else {
                        pending_function_name = match[1].str();
                        awaiting_open_brace = true;
                    }
                }
            }

            for (const char letter : line) {
                if (letter == '{') {
                    ++brace_depth;
                } else if (letter == '}') {
                    if (brace_depth > 0) { --brace_depth; }
                    if (brace_depth == 0) { current_function.clear(); }
                }
            }

            for (const char *guarded_call : kGuardedCalls) {
                if (line.find(guarded_call) == std::string::npos) { continue; }
                ++matches_found;
                const bool is_allowed =
                  relative_file == kAllowedFile && current_function.find(kAllowedFunction) != std::string::npos;
                if (!is_allowed) {
                    violations.push_back(relative_file + ":" + std::to_string(line_number) + ": " + line);
                }
            }
        }
    }

    ASSERT_GT(matches_found, 0u) << "found zero asManager.createASForScene(...)/asManager.createTLAS(...) call "
                                    "sites under Src/ - the scan itself is broken, or every call site was removed "
                                    "and this gate is now vacuous";

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " acceleration-structure rebuild(s) bypass VulkanRenderer::refreshAfterSceneChange(), the only place "
         "that follows a rebuild with updateAllDescriptorSets() - route through it instead of calling "
         "asManager.createASForScene()/createTLAS() directly:"
      << joinViolations(violations);

    const auto header_contents_opt =
      readFileText(repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "VulkanRenderer.ixx");
    ASSERT_TRUE(header_contents_opt.has_value()) << "missing VulkanRenderer.ixx";
    const std::string &header_contents = *header_contents_opt;
    ASSERT_NE(header_contents.find("void refreshAfterSceneChange(bool rebuildBottomLevel);"), std::string::npos)
      << "VulkanRenderer::refreshAfterSceneChange is no longer declared in VulkanRenderer.ixx - this gate's "
         "allow-list needs updating if it was renamed or its signature changed";
}

// Feature bits come from availability queries; only the proven hardware-RT block may hardcode true, or startup fails.
TEST(BuildIntegrity, EveryEnabledDeviceFeatureIsCopiedFromAnAvailabilityQuery)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path vulkan_device_cpp = repo_root / "Src" / "GraphicsEngineVulkan" / "vulkan_base" / "VulkanDevice.cpp";
    const auto lines_opt = readFileLines(vulkan_device_cpp);
    ASSERT_TRUE(lines_opt.has_value()) << "could not open " << vulkan_device_cpp.string();
    const auto &lines = *lines_opt;

    static const std::string kGuard = "if (deviceSupportsHardwareAcceleratedRRT)";
    std::size_t guard_start = lines.size();
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (lines[index].find(kGuard) != std::string::npos) {
            guard_start = index;
            break;
        }
    }
    ASSERT_LT(guard_start, lines.size()) << vulkan_device_cpp.string() << " has no " << kGuard
                                         << " block to check - did the guard get renamed?";

    std::size_t guard_end = lines.size();
    int brace_depth = 0;
    bool guard_body_started = false;
    for (std::size_t index = guard_start; index < lines.size(); ++index) {
        brace_depth += static_cast<int>(std::count(lines[index].begin(), lines[index].end(), '{'));
        brace_depth -= static_cast<int>(std::count(lines[index].begin(), lines[index].end(), '}'));
        if (brace_depth > 0) { guard_body_started = true; }
        if (guard_body_started && brace_depth <= 0) {
            guard_end = index;
            break;
        }
    }
    ASSERT_LT(guard_end, lines.size()) << "could not find the closing '}' of the " << kGuard << " block";

    static const std::regex kHardcodedFeatureBit(
      R"((features1[123]\.\w+|features2\.features\.\w+)\s*=\s*(VK_TRUE|true)\b)");

    std::vector<std::string> violations;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index >= guard_start && index <= guard_end) { continue; }
        if (std::regex_search(lines[index], kHardcodedFeatureBit)) {
            violations.push_back("VulkanDevice.cpp:" + std::to_string(index + 1) + ": " + lines[index]);
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size() << " device feature bit(s) are hardcoded to true outside the " << kGuard
      << " block - every enabled feature must be copied from an availability query so a device that does not "
         "support it degrades instead of failing vkCreateDevice:"
      << joinViolations(violations);
}

// An image-count change must reinit the shadow map too, after initDescriptorResources() recreates the layout it caches.
TEST(BuildIntegrity, EveryPerSwapchainImageSubsystemIsReprovisionedOnImageCountChange)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path renderer_path = repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "VulkanRenderer.cpp";
    const auto renderer_contents_opt = readFileText(renderer_path);
    ASSERT_TRUE(renderer_contents_opt.has_value()) << "could not open " << renderer_path.string();
    const std::string &renderer_contents = *renderer_contents_opt;

    const std::size_t reinit_occurrences = [&renderer_contents] {
        std::size_t count = 0;
        std::size_t pos = 0;
        while ((pos = renderer_contents.find("reinitShadowMapForCurrentSettings", pos)) != std::string::npos) {
            ++count;
            pos += std::string("reinitShadowMapForCurrentSettings").size();
        }
        return count;
    }();

    EXPECT_GE(reinit_occurrences, 3u)
      << "found only " << reinit_occurrences << " occurrence(s) of reinitShadowMapForCurrentSettings in "
      << renderer_path.string()
      << " - expected at least 3 (the definition plus a call from handleShadowResolutionChange and a call from "
         "reprovisionPerImageResources); deleting either call site regresses the swapchain-image-count-change bug "
         "this test guards against.";

    const std::size_t signature_pos = renderer_contents.find("VulkanRenderer::reprovisionPerImageResources(");
    ASSERT_NE(signature_pos, std::string::npos)
      << "VulkanRenderer::reprovisionPerImageResources is no longer defined in " << renderer_path.string();

    const std::size_t body_open = renderer_contents.find('{', signature_pos);
    ASSERT_NE(body_open, std::string::npos)
      << "could not locate the opening brace of VulkanRenderer::reprovisionPerImageResources";

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
      << "could not brace-match the closing '}' of VulkanRenderer::reprovisionPerImageResources";

    const std::string body = renderer_contents.substr(body_open, body_close - body_open + 1);

    const std::size_t init_descriptor_pos = body.find("initDescriptorResources();");
    ASSERT_NE(init_descriptor_pos, std::string::npos)
      << "VulkanRenderer::reprovisionPerImageResources no longer calls initDescriptorResources() - "
         "reinitShadowMapForCurrentSettings must run after it, since CascadedShadowMap::init caches the "
         "sharedRenderDescriptors layout that call (re)creates.";

    const std::size_t reinit_call_pos = body.find("reinitShadowMapForCurrentSettings();");
    ASSERT_NE(reinit_call_pos, std::string::npos)
      << "VulkanRenderer::reprovisionPerImageResources no longer calls reinitShadowMapForCurrentSettings() - "
         "without it, dirShadowMap is never re-provisioned when the swapchain image count changes, so the shadow "
         "pass silently stops rendering for the added images.";

    EXPECT_GT(reinit_call_pos, init_descriptor_pos)
      << "reinitShadowMapForCurrentSettings() is called before initDescriptorResources() inside "
         "reprovisionPerImageResources - CascadedShadowMap::init caches the sharedRenderDescriptors layout handed "
         "to it, so re-initing before initDescriptorResources() replaces that layout would leave it caching the "
         "layout that is about to be destroyed.";
}

// Only the renderer owns a command pool, and the shadow map seeds its host-visible buffers without staging.
TEST(BuildIntegrity, OnlyTheRendererCreatesACommandPool)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path engine_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(sourceExists(engine_root)) << "missing " << engine_root.string();

    std::vector<std::string> files_with_pool_creation;
    std::error_code error;
    for (fs::recursive_directory_iterator it(engine_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx") { continue; }

        const auto contents = readFileText(path);
        if (!contents) { continue; }
        if (contents->find("createCommandPool(") == std::string::npos) { continue; }

        files_with_pool_creation.push_back(fs::relative(path, repo_root).generic_string());
    }

    static const std::array<const char *, 1> kExpected = { "Src/GraphicsEngineVulkan/renderer/VulkanRenderer.cpp" };
    std::sort(files_with_pool_creation.begin(), files_with_pool_creation.end());

    EXPECT_TRUE(
      std::equal(files_with_pool_creation.begin(), files_with_pool_creation.end(), kExpected.begin(), kExpected.end()))
      << "expected only VulkanRenderer.cpp to create a vk::CommandPool, found:"
      << joinViolations(files_with_pool_creation);
}
