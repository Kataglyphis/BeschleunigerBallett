// Build integrity: Vulkan results: creation checked, command buffers and submits consumed, acquire retired.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "BuildIntegrityScan.hpp"

using namespace Kataglyphis::TestSupport::BuildIntegrity;

namespace {

// A justified exception; its marker must trail the exempted line, so it cannot rot after the line moves.
struct AllowlistEntry
{
    std::string file;
    std::string marker;
};

// Empty: an explicit ".result" check near a ".value" read already satisfies the gate.
const std::vector<AllowlistEntry> kCheckedResultAllowlist = {};

const std::string kUncheckedResultMarkerPrefix = "UNCHECKED_VULKAN_RESULT_OK: ";

// Matched by marker text, not line number, so an edit above an exempted line cannot shift the exemption.
int allowlisted_result_check_index(const std::string &relative_file, const std::string &line)
{
    for (std::size_t idx = 0; idx < kCheckedResultAllowlist.size(); ++idx) {
        const AllowlistEntry &entry = kCheckedResultAllowlist[idx];
        if (entry.file != relative_file) { continue; }
        if (line.find(kUncheckedResultMarkerPrefix + entry.marker) != std::string::npos) {
            return static_cast<int>(idx);
        }
    }
    return -1;
}

// A create or allocate keyword at a word start, then an uppercase letter (camelCase, not snake_case), then '('.
bool looks_like_creation_call(const std::string &line)
{
    static const std::vector<std::string> keywords = { "create", "Create", "allocate", "Allocate" };
    for (const auto &keyword : keywords) {
        std::size_t pos = 0;
        while ((pos = line.find(keyword, pos)) != std::string::npos) {
            const bool left_ok = pos == 0 || !is_identifier_char(line[pos - 1]);
            const std::size_t after_keyword = pos + keyword.size();
            const bool camel_case_continuation =
              after_keyword < line.size() && std::isupper(static_cast<unsigned char>(line[after_keyword])) != 0;
            if (left_ok && camel_case_continuation) {
                std::size_t scan = after_keyword;
                while (scan < line.size() && is_identifier_char(line[scan])) { ++scan; }
                while (scan < line.size() && std::isspace(static_cast<unsigned char>(line[scan])) != 0) { ++scan; }
                if (scan < line.size() && line[scan] == '(') { return true; }
            }
            pos += keyword.size();
        }
    }
    return false;
}

// The same for query verbs, which return vk::ResultValue too but slip past the creation matcher.
bool looks_like_query_call(const std::string &line)
{
    static const std::vector<std::string> keywords = { "get", "Get", "enumerate", "Enumerate" };
    for (const auto &keyword : keywords) {
        std::size_t pos = 0;
        while ((pos = line.find(keyword, pos)) != std::string::npos) {
            const bool left_ok = pos == 0 || !is_identifier_char(line[pos - 1]);
            const std::size_t after_keyword = pos + keyword.size();
            const bool camel_case_continuation =
              after_keyword < line.size() && std::isupper(static_cast<unsigned char>(line[after_keyword])) != 0;
            if (left_ok && camel_case_continuation) {
                std::size_t scan = after_keyword;
                while (scan < line.size() && is_identifier_char(line[scan])) { ++scan; }
                while (scan < line.size() && std::isspace(static_cast<unsigned char>(line[scan])) != 0) { ++scan; }
                if (scan < line.size() && line[scan] == '(') { return true; }
            }
            pos += keyword.size();
        }
    }
    return false;
}

}// namespace

// Exceptions are off, so each ".value" read needs a vkCheck in a window on both sides (else-branches sit below).
TEST(BuildIntegrity, VulkanCreationResultsAreChecked)
{
    // Prove the matcher fires first, so a neutered one cannot report zero violations forever.
    EXPECT_TRUE(looks_like_query_call("  auto x = d.getSurfaceFormatsKHR(*s).value;"));
    EXPECT_FALSE(looks_like_query_call("  auto x = getter(y).value;"));

    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path engine_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(sourceExists(engine_root)) << "missing " << engine_root.string();

    constexpr int kWindow = 8;
    std::vector<std::string> violations;
    std::vector<bool> allowlist_entry_matched(kCheckedResultAllowlist.size(), false);
    std::error_code error;
    for (fs::recursive_directory_iterator it(engine_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || path.extension() != ".cpp") { continue; }

        const auto lines_opt = readFileLines(path);
        if (!lines_opt) { continue; }
        const auto &lines = *lines_opt;

        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].find(".value") == std::string::npos) { continue; }

            const std::size_t window_begin = (i >= static_cast<std::size_t>(kWindow)) ? i - kWindow : 0;
            const std::size_t window_end = std::min(lines.size() - 1, i + static_cast<std::size_t>(kWindow));

            bool triggered = false;
            bool asserted = false;
            for (std::size_t w = window_begin; w <= window_end; ++w) {
                if (w <= i && (looks_like_creation_call(lines[w]) || looks_like_query_call(lines[w]))) {
                    triggered = true;
                }
                if (lines[w].find("vkCheck(") != std::string::npos) { asserted = true; }
                // Queries may check ".result" explicitly, but never on the ".value" line itself.
                if (w != i && lines[w].find(".result") != std::string::npos) { asserted = true; }
            }
            if (!triggered || asserted) { continue; }

            const std::string relative_file = fs::relative(path, engine_root).generic_string();
            const int allowlist_index = allowlisted_result_check_index(relative_file, lines[i]);
            if (allowlist_index >= 0) {
                allowlist_entry_matched[static_cast<std::size_t>(allowlist_index)] = true;
                continue;
            }

            violations.push_back(relative_file + ":" + std::to_string(i + 1) + ": " + lines[i]);
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " Vulkan creation/allocation/query result(s) read via .value with no vkCheck and no "
         "\".result\" check nearby (exceptions are disabled project-wide, so a failed call must either "
         "abort via vkCheck or be handled explicitly via an `if (x.result != ...)` check rather "
         "than continue with a silently default/empty value; or, for a deliberate exception, add a "
         "\"// UNCHECKED_VULKAN_RESULT_OK: <marker>\" comment on the line and a justified entry "
         "to kCheckedResultAllowlist above):"
      << joinViolations(violations);

    std::vector<std::string> dead_exemptions;
    for (std::size_t idx = 0; idx < kCheckedResultAllowlist.size(); ++idx) {
        if (!allowlist_entry_matched[idx]) {
            dead_exemptions.push_back(
              kCheckedResultAllowlist[idx].file + " (" + kCheckedResultAllowlist[idx].marker + ")");
        }
    }

    EXPECT_TRUE(dead_exemptions.empty())
      << dead_exemptions.size()
      << " kCheckedResultAllowlist entr(y/ies) matched no \"// UNCHECKED_VULKAN_RESULT_OK: <marker>\" "
         "comment in the source - the exemption is dead and must be deleted:"
      << joinViolations(dead_exemptions);
}

namespace {

// The one non-call "beginCommandBuffer(" under Src/ is its own definition.
bool is_begin_command_buffer_definition_line(const std::string &line)
{ return line.find("beginCommandBuffer(vk::Device device") != std::string::npos; }

// The name declared as "vk::CommandBuffer <name>" on the call line or up to `lookback` lines above, else empty.
std::string declared_command_buffer_name(const std::vector<std::string> &lines, std::size_t call_line, int lookback)
{
    static const std::string marker = "vk::CommandBuffer ";
    const std::size_t begin =
      (call_line >= static_cast<std::size_t>(lookback)) ? call_line - static_cast<std::size_t>(lookback) : 0;
    for (std::size_t w = call_line + 1; w-- > begin;) {
        const std::string &line = lines[w];
        const std::size_t marker_pos = line.find(marker);
        if (marker_pos == std::string::npos) { continue; }
        const std::size_t name_begin = marker_pos + marker.size();
        std::size_t name_end = name_begin;
        while (name_end < line.size() && is_identifier_char(line[name_end])) { ++name_end; }
        if (name_end > name_begin) { return line.substr(name_begin, name_end - name_begin); }
    }
    return "";
}

}// namespace

// A null command buffer is beginCommandBuffer's only failure signal, so each result needs a null check.
TEST(BuildIntegrity, EveryBeginCommandBufferResultIsChecked)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(sourceExists(src_root)) << "missing " << src_root.string();

    constexpr int kLookback = 3;
    constexpr int kWindow = 6;
    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || path.extension() != ".cpp") { continue; }

        const auto lines_opt = readFileLines(path);
        if (!lines_opt) { continue; }
        const auto &lines = *lines_opt;

        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].find("beginCommandBuffer(") == std::string::npos) { continue; }
            if (is_begin_command_buffer_definition_line(lines[i])) { continue; }

            const std::string relative_file = fs::relative(path, repo_root).generic_string();
            const std::string var_name = declared_command_buffer_name(lines, i, kLookback);
            if (var_name.empty()) {
                violations.push_back(relative_file + ":" + std::to_string(i + 1)
                  + ": beginCommandBuffer( call whose assigned vk::CommandBuffer variable could not be located - "
                    "cannot verify a null-check exists");
                continue;
            }

            const std::string negated_check = "!" + var_name;
            const std::size_t window_end = std::min(lines.size() - 1, i + static_cast<std::size_t>(kWindow));
            bool checked = false;
            for (std::size_t w = i; w <= window_end; ++w) {
                if (lines[w].find(negated_check) != std::string::npos) {
                    checked = true;
                    break;
                }
            }

            if (!checked) { violations.push_back(relative_file + ":" + std::to_string(i + 1) + ": " + lines[i]); }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size() << " beginCommandBuffer() call(s) with no null-check on the returned command buffer within "
      << kWindow
      << " lines - beginCommandBuffer documents a null return on allocate/begin failure, and exceptions are "
         "disabled project-wide, so that null handle is the only failure signal available; recording into it "
         "or submitting it is undefined behaviour:"
      << joinViolations(violations);
}

// Each submit result is assigned, tested, or discarded via static_cast<void>, so a failure is never invisible.
TEST(BuildIntegrity, EveryEndAndSubmitCommandBufferResultIsChecked)
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
        if (!it->is_regular_file(error) || path.extension() != ".cpp") { continue; }

        const auto lines_opt = readFileLines(path);
        if (!lines_opt) { continue; }
        const auto &lines = *lines_opt;

        for (std::size_t i = 0; i < lines.size(); ++i) {
            const std::string &line = lines[i];
            const std::size_t call_pos = line.find("endAndSubmitCommandBuffer(");
            if (call_pos == std::string::npos) { continue; }
            // The function's own definition signature, not a call.
            if (line.find("endAndSubmitCommandBuffer(vk::Device device") != std::string::npos) { continue; }

            const std::string relative_file = fs::relative(path, repo_root).generic_string();
            const std::string prefix = line.substr(0, call_pos);

            const bool discarded = prefix.find("static_cast<void>(") != std::string::npos;
            const bool assigned = prefix.find(" = ") != std::string::npos;
            const bool in_condition = prefix.find("if (") != std::string::npos
                                      || prefix.find("if(") != std::string::npos
                                      || prefix.find("while (") != std::string::npos;

            if (!discarded && !assigned && !in_condition) {
                violations.push_back(relative_file + ":" + std::to_string(i + 1) + ": " + line);
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " endAndSubmitCommandBuffer(...) call(s) whose bool result is neither assigned, used in a condition, "
         "nor explicitly static_cast<void>-discarded - a failed submit must not be silently ignored:"
      << joinViolations(violations);
}

// A checked null is not a handled failure: createTLAS must not index a short BLAS vector, nor clouds hand out null.
TEST(BuildIntegrity, CommandBufferFailurePathsDoNotLeaveHalfBuiltResources)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path as_manager_path =
      repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "accelerationStructures" / "ASManager.cpp";
    const auto as_manager_source_opt = readFileText(as_manager_path);
    ASSERT_TRUE(as_manager_source_opt.has_value()) << "missing " << as_manager_path.string();
    const std::string &as_manager_source = *as_manager_source_opt;

    EXPECT_NE(
      as_manager_source.find("bool Kataglyphis::VulkanRendererInternals::ASManager::createBLAS("), std::string::npos)
      << "ASManager::createBLAS must be declared returning bool so its caller can react to a failed build";

    {
        const std::string needle = "if (!createBLAS(";
        EXPECT_NE(as_manager_source.find(needle), std::string::npos)
          << "ASManager::createASForScene must guard its createBLAS(...) call with an if (!...) check";
    }

    {
        const std::size_t create_tlas_pos =
          as_manager_source.find("Kataglyphis::VulkanRendererInternals::ASManager::createTLAS(");
        ASSERT_NE(create_tlas_pos, std::string::npos) << "could not locate ASManager::createTLAS definition";
        const std::size_t body_start = as_manager_source.find('{', create_tlas_pos);
        ASSERT_NE(body_start, std::string::npos);
        const std::size_t first_index = as_manager_source.find("blas[", body_start);
        ASSERT_NE(first_index, std::string::npos) << "createTLAS no longer indexes blas[...]; update this test";
        const std::size_t size_guard = as_manager_source.find("blas.size()", body_start);
        EXPECT_NE(size_guard, std::string::npos) << "createTLAS must check blas.size() before indexing blas[...]";
        EXPECT_LT(size_guard, first_index)
          << "createTLAS's blas.size() guard must appear before the first blas[...] index";
    }

    const fs::path clouds_path =
      repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "atmospheric_effects" / "clouds" / "Clouds.cpp";
    const auto clouds_source_opt = readFileText(clouds_path);
    ASSERT_TRUE(clouds_source_opt.has_value()) << "missing " << clouds_path.string();
    const std::string &clouds_source = *clouds_source_opt;

    EXPECT_EQ(clouds_source.find("return nullptr;"), std::string::npos)
      << "Clouds.cpp must not return a null texture from createStorageTexture - a half-initialized clouds "
         "subsystem has no defined rendering behaviour, so a failed command buffer must vkFail instead";
}

// uploadRgba must consume the submit result, or a failed upload reports success with an unwritten image bound.
TEST(BuildIntegrity, TextureUploadConsumesTheSubmitResult)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path texture_cpp_path = repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "Texture.cpp";
    const auto texture_cpp_source_opt = readFileText(texture_cpp_path);
    ASSERT_TRUE(texture_cpp_source_opt.has_value()) << "missing " << texture_cpp_path.string();
    const std::string &texture_cpp_source = *texture_cpp_source_opt;

    const std::size_t call_pos = texture_cpp_source.find("endAndSubmitCommandBuffer(");
    ASSERT_NE(call_pos, std::string::npos) << "could not locate endAndSubmitCommandBuffer( call in Texture.cpp";
    const std::size_t line_start = texture_cpp_source.rfind('\n', call_pos);
    const std::string prefix =
      texture_cpp_source.substr(line_start == std::string::npos ? 0 : line_start + 1, call_pos - (line_start + 1));
    EXPECT_EQ(prefix.find("static_cast<void>("), std::string::npos)
      << "Texture.cpp's endAndSubmitCommandBuffer(...) call must consume the submit result instead of "
         "discarding it with static_cast<void> - a failed upload must not be reported as a successful texture";

    const fs::path texture_ixx_path = repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "Texture.ixx";
    const auto texture_ixx_source_opt = readFileText(texture_ixx_path);
    ASSERT_TRUE(texture_ixx_source_opt.has_value()) << "missing " << texture_ixx_path.string();
    const std::string &texture_ixx_source = *texture_ixx_source_opt;

    EXPECT_NE(texture_ixx_source.find("bool createDefaultTexture("), std::string::npos)
      << "Texture::createDefaultTexture must be declared returning bool, forwarding uploadRgba's failure to "
         "its callers instead of swallowing it as void";
}

// Buffer and cubemap uploads must surface a failed submit, or shaders read undefined contents as geometry.
TEST(BuildIntegrity, GeometryAndCubemapUploadsConsumeTheSubmitResult)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path buffer_manager_cpp_path =
      repo_root / "Src" / "GraphicsEngineVulkan" / "vulkan_base" / "VulkanBufferManager.cpp";
    const auto buffer_manager_cpp_source_opt = readFileText(buffer_manager_cpp_path);
    ASSERT_TRUE(buffer_manager_cpp_source_opt.has_value()) << "missing " << buffer_manager_cpp_path.string();
    const std::string &buffer_manager_cpp_source = *buffer_manager_cpp_source_opt;

    {
        const std::size_t call_pos = buffer_manager_cpp_source.find("endAndSubmitCommandBuffer(");
        ASSERT_NE(call_pos, std::string::npos)
          << "could not locate endAndSubmitCommandBuffer( call in VulkanBufferManager.cpp";
        const std::size_t line_start = buffer_manager_cpp_source.rfind('\n', call_pos);
        const std::string prefix = buffer_manager_cpp_source.substr(
          line_start == std::string::npos ? 0 : line_start + 1, call_pos - (line_start + 1));
        EXPECT_EQ(prefix.find("static_cast<void>("), std::string::npos)
          << "VulkanBufferManager.cpp's endAndSubmitCommandBuffer(...) call must consume the submit result "
             "instead of discarding it with static_cast<void> - a failed transfer must not be reported as a "
             "successful upload";
    }

    const fs::path sky_box_cpp_path = repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "sky_box" / "SkyBox.cpp";
    const auto sky_box_cpp_source_opt = readFileText(sky_box_cpp_path);
    ASSERT_TRUE(sky_box_cpp_source_opt.has_value()) << "missing " << sky_box_cpp_path.string();
    const std::string &sky_box_cpp_source = *sky_box_cpp_source_opt;

    {
        const std::size_t call_pos = sky_box_cpp_source.find("endAndSubmitCommandBuffer(");
        ASSERT_NE(call_pos, std::string::npos) << "could not locate endAndSubmitCommandBuffer( call in SkyBox.cpp";
        const std::size_t line_start = sky_box_cpp_source.rfind('\n', call_pos);
        const std::string prefix =
          sky_box_cpp_source.substr(line_start == std::string::npos ? 0 : line_start + 1, call_pos - (line_start + 1));
        EXPECT_EQ(prefix.find("static_cast<void>("), std::string::npos)
          << "SkyBox.cpp's endAndSubmitCommandBuffer(...) call must consume the submit result instead of "
             "discarding it with static_cast<void> - a failed cubemap upload must not write a descriptor "
             "pointed at an unfilled image";
    }

    const fs::path buffer_manager_ixx_path =
      repo_root / "Src" / "GraphicsEngineVulkan" / "vulkan_base" / "VulkanBufferManager.ixx";
    const auto buffer_manager_ixx_source_opt = readFileText(buffer_manager_ixx_path);
    ASSERT_TRUE(buffer_manager_ixx_source_opt.has_value()) << "missing " << buffer_manager_ixx_path.string();
    const std::string &buffer_manager_ixx_source = *buffer_manager_ixx_source_opt;

    EXPECT_NE(buffer_manager_ixx_source.find("bool copyBuffer("), std::string::npos)
      << "VulkanBufferManager::copyBuffer must be declared returning bool";
    EXPECT_NE(buffer_manager_ixx_source.find("bool createBufferAndUploadVectorOnDevice("), std::string::npos)
      << "VulkanBufferManager::createBufferAndUploadVectorOnDevice must be declared returning bool";
    EXPECT_EQ(buffer_manager_ixx_source.find("copyImageBuffer(vk::Device device,"), std::string::npos)
      << "the dead seven-argument device overload of copyImageBuffer must be deleted - only the static "
         "command-buffer overload should remain";
}

// A post-acquire early return must go through an abort helper, or the next acquire reuses a signaled semaphore.
TEST(BuildIntegrity, EveryPostAcquireEarlyReturnRetiresTheAcquireSemaphore)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path renderer_path = repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "VulkanRenderer.cpp";
    const auto renderer_lines = readFileLines(renderer_path);
    ASSERT_TRUE(renderer_lines.has_value()) << "missing " << renderer_path.string();
    const auto &lines = *renderer_lines;

    std::size_t acquire_line = lines.size();
    std::size_t advance_frame_line = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (acquire_line == lines.size() && lines[i].find("acquireNextImageKHR(") != std::string::npos) {
            acquire_line = i;
        }
        if (lines[i].find("frameSync.advanceFrame();") != std::string::npos) {
            advance_frame_line = i;
            break;
        }
    }
    ASSERT_NE(acquire_line, lines.size()) << "could not locate the acquireNextImageKHR( call in drawFrame";
    ASSERT_NE(advance_frame_line, lines.size()) << "could not locate the frameSync.advanceFrame(); call in drawFrame";
    ASSERT_LT(acquire_line, advance_frame_line)
      << "acquireNextImageKHR( must appear before frameSync.advanceFrame(); - drawFrame was restructured; "
         "update this test";

    const auto trim = [](const std::string &s) -> std::string {
        const std::size_t begin = s.find_first_not_of(" \t");
        if (begin == std::string::npos) { return ""; }
        const std::size_t end = s.find_last_not_of(" \t");
        return s.substr(begin, end - begin + 1);
    };

    constexpr int kLookback = 3;
    std::vector<std::string> violations;
    for (std::size_t i = acquire_line; i <= advance_frame_line; ++i) {
        if (trim(lines[i]) != "return;") { continue; }

        bool retires_semaphore = false;
        int checked = 0;
        for (std::size_t w = i; w-- > 0 && checked < kLookback;) {
            if (trim(lines[w]).empty()) { continue; }
            ++checked;
            if (lines[w].find("abort_frame_after_acquire(") != std::string::npos
                || lines[w].find("abort_frame_with_fatal_error(") != std::string::npos) {
                retires_semaphore = true;
                break;
            }
        }

        if (!retires_semaphore) {
            violations.push_back(
              "VulkanRenderer.cpp:" + std::to_string(i + 1)
              + ": bare \"return;\" in drawFrame's post-acquire span with no abort_frame_after_acquire(/"
                "abort_frame_with_fatal_error( call in the previous " + std::to_string(kLookback) + " non-blank lines");
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " post-acquire early return(s) in drawFrame do not retire imageAvailableSemaphore() before returning "
         "(via abort_frame_after_acquire( or abort_frame_with_fatal_error():"
      << joinViolations(violations);
}

// vkCheck replaced the ASSERT_VULKAN macro's check-then-read-.value idiom; a new site must not bring the macro back.
TEST(BuildIntegrity, VulkanResultsGoThroughVkCheckNotTheRetiredMacro)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path utilities = repo_root / "Src" / "GraphicsEngineVulkan" / "common" / "Utilities.hpp";
    const auto utilities_text = readFileText(utilities);
    ASSERT_TRUE(utilities_text.has_value()) << "missing " << utilities.string();
    EXPECT_NE(utilities_text->find("auto vkCheck(vk::ResultValue<T> &&"), std::string::npos)
      << "Utilities.hpp no longer defines the ResultValue-unwrapping vkCheck";

    static const std::array<fs::path, 2> kRoots = { repo_root / "Src" / "GraphicsEngineVulkan",
        repo_root / "Src" / "shared" };
    // Split so this file does not match its own needle.
    const std::string kRetiredMacro = std::string("ASSERT_") + "VULKAN";

    std::vector<std::string> violations;
    std::size_t vk_check_sites = 0;
    std::error_code error;
    for (const fs::path &root : kRoots) {
        if (!sourceExists(root)) { continue; }
        for (fs::recursive_directory_iterator it(root, error), end; it != end; it.increment(error)) {
            if (error) { break; }
            const fs::path &path = it->path();
            if (!it->is_regular_file(error)) { continue; }
            const auto extension = path.extension();
            if (extension != ".hpp" && extension != ".cpp" && extension != ".ixx") { continue; }

            const auto lines = readFileLines(path);
            if (!lines) { continue; }
            std::size_t line_number = 0;
            for (const auto &line : *lines) {
                ++line_number;
                if (line.find("vkCheck(") != std::string::npos) { ++vk_check_sites; }
                if (line.find(kRetiredMacro) == std::string::npos) { continue; }
                violations.push_back(
                  fs::relative(path, repo_root).generic_string() + ":" + std::to_string(line_number) + ": " + line);
            }
        }
    }

    // The sweep left 38 vkCheck lines; far fewer means the walk or the needle broke, not that the code changed.
    EXPECT_GE(vk_check_sites, 30U) << "found only " << vk_check_sites << " vkCheck( lines under Src/";
    EXPECT_TRUE(violations.empty()) << violations.size() << " line(s) still name the retired " << kRetiredMacro
                                    << " macro; unwrap with vkCheck(call, \"message\") from common/Utilities.hpp:"
                                    << joinViolations(violations);
}

// The submit-failure context once named the GUI's pick, ray tracing first, while a TLAS-less frame drew raster.
TEST(BuildIntegrity, SubmitFailureLogNamesTheDrawnMode)
{
    const fs::path renderer_path = repoRoot() / "Src" / "GraphicsEngineVulkan" / "renderer" / "VulkanRenderer.cpp";
    const auto text = readFileText(renderer_path);
    ASSERT_TRUE(text.has_value()) << "could not open " << renderer_path.string();
    EXPECT_NE(text->find("renderModeLogName(drawnRenderMode())"), std::string::npos)
      << renderer_path.string() << " no longer names drawnRenderMode() in its queue-submit context";
}

// A false submit means nothing ran; discarding it reads failure as success unless SUBMIT_RESULT_IGNORED_OK marks it.
TEST(BuildIntegrity, EverySubmitResultIsCheckedOrExplicitlyExempt)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(sourceExists(src_root)) << "missing " << src_root.string();

    static const char *const kCallMarker = "endAndSubmitCommandBuffer";
    static const char *const kDiscardMarker = "static_cast<void>(";
    static const char *const kExemptionPrefix = "SUBMIT_RESULT_IGNORED_OK: ";

    std::vector<std::string> violations;
    bool call_site_seen = false;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".hpp" && path.extension() != ".ixx") { continue; }

        const std::string relative_file = fs::relative(path, repo_root).generic_string();

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        std::size_t line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            if (line.find(kCallMarker) == std::string::npos) { continue; }
            if (line.find(kDiscardMarker) == std::string::npos) {
                call_site_seen = true;
                continue;
            }
            if (line.find(kExemptionPrefix) != std::string::npos) { continue; }
            violations.push_back(relative_file + ":" + std::to_string(line_number) + ": " + line);
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " endAndSubmitCommandBuffer() call(s) found under Src/ whose [[nodiscard]] result is thrown away with "
         "static_cast<void>(...) - capture the bool and handle the false case (it means the submitted work "
         "never happened), or add a trailing \"// SUBMIT_RESULT_IGNORED_OK: <reason>\" comment on the line if "
         "the discard is a deliberate, reasoned exception:"
      << joinViolations(violations);

    EXPECT_TRUE(call_site_seen) << "expected to find at least one non-discarded endAndSubmitCommandBuffer() call "
                                   "site under Src/ - if this fired, either the function was renamed (update "
                                   "kCallMarker) or every call site started discarding its result, and this gate "
                                   "would be silently checking nothing";
}
