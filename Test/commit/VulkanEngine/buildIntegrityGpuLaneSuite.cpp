// Build integrity: every lane that runs the GPU suites excludes, guards and fails a skip as documented.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "BuildIntegrityScan.hpp"

using namespace Kataglyphis::TestSupport::BuildIntegrity;

namespace {

// Every GTest suite name under tests_dir; anchored at line start, so comments and string literals do not count.
std::set<std::string> collect_defined_suites(const fs::path &tests_dir)
{
    std::set<std::string> suites;
    std::error_code error;
    for (fs::recursive_directory_iterator it(tests_dir, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error) || it->path().extension() != ".cpp") { continue; }

        const auto lines = readFileLines(it->path());
        if (!lines) { continue; }
        for (const auto &line : *lines) {
            const std::size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos) { continue; }

            for (const std::string &macro : { std::string("TEST_F("), std::string("TEST(") }) {
                if (line.compare(start, macro.size(), macro) != 0) { continue; }

                const std::size_t name_start = start + macro.size();
                const std::size_t comma = line.find(',', name_start);
                if (comma == std::string::npos) { break; }

                const std::size_t name_begin = line.find_first_not_of(" \t", name_start);
                const std::size_t name_end = line.find_last_not_of(" \t", comma - 1);
                if (name_begin == std::string::npos || name_begin > name_end) { break; }

                suites.insert(line.substr(name_begin, name_end - name_begin + 1));
                break;
            }
        }
    }
    return suites;
}

// Suite globs of Invoke-WindowsLane.ps1's $gpuOnlySuites array, anchored on its opener and -join closer.
std::optional<std::vector<std::string>> parse_ci_gpu_excluded_suites(const fs::path &lane_path)
{
    const auto lines = readFileLines(lane_path);
    if (!lines) { return std::nullopt; }

    std::vector<std::string> suites;
    bool inside_array = false;
    for (const auto &line : *lines) {
        if (!inside_array) {
            if (line.find("$gpuOnlySuites = @(") != std::string::npos) { inside_array = true; }
            continue;
        }
        if (line.find("-join ':'") != std::string::npos) { break; }

        const std::size_t open_quote = line.find('\'');
        if (open_quote == std::string::npos) { continue; }
        const std::size_t close_quote = line.find('\'', open_quote + 1);
        if (close_quote == std::string::npos) { continue; }

        std::string entry = line.substr(open_quote + 1, close_quote - open_quote - 1);
        static const std::string kGlobSuffix = ".*";
        if (entry.size() > kGlobSuffix.size()
            && entry.compare(entry.size() - kGlobSuffix.size(), kGlobSuffix.size(), kGlobSuffix) == 0) {
            entry.erase(entry.size() - kGlobSuffix.size());
        }
        suites.push_back(entry);
    }
    return suites;
}

}// namespace

// The Windows lane runs every suite but the excluded GPU ones, so only that exclusion list can drift.
TEST(BuildIntegrity, WindowsCiExcludesExactlyTheGpuSuites)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path lane_path = repo_root / "scripts" / "windows" / "Invoke-WindowsLane.ps1";
    const auto filter_suites_opt = parse_ci_gpu_excluded_suites(lane_path);
    if (!filter_suites_opt.has_value()) {
        GTEST_SKIP() << "could not open " << lane_path.string() << " - not running from the repo root?";
    }
    const std::vector<std::string> &filter_suites = *filter_suites_opt;
    ASSERT_FALSE(filter_suites.empty())
      << "parsed zero suites out of the $gpuOnlySuites array in " << lane_path.string()
      << " - the anchor text ('$gpuOnlySuites = @(' / \"-join ':'\") may have changed";
    const std::set<std::string> filter_set(filter_suites.begin(), filter_suites.end());

    const std::set<std::string> defined_suites = collect_defined_suites(repo_root / "Test" / "commit" / "VulkanEngine");
    ASSERT_FALSE(defined_suites.empty()) << "found zero TEST()/TEST_F() suites under Test/commit/VulkanEngine - "
                                            "the scan itself is broken";

    // The container has a Vulkan loader but no GPU, so these abort in device creation instead of skipping.
    const std::set<std::string> gpu_excluded_suites = { "GoldenRender", "Integration" };

    EXPECT_EQ(filter_set, gpu_excluded_suites)
      << "Invoke-WindowsLane.ps1's $gpuOnlySuites exclusion list no longer matches the expected GPU-suite set - "
         "update either the lane script or this test's gpu_excluded_suites";

    for (const auto &suite : filter_suites) {
        EXPECT_TRUE(defined_suites.contains(suite))
          << "Invoke-WindowsLane.ps1 excludes '" << suite
          << "' from $gpuOnlySuites, but no such suite is defined under Test/commit/VulkanEngine "
             "(renamed or deleted?)";
    }
}

// KATAGLYPHIS_REQUIRE_GPU reaches a GPU test only through GpuTestGuard.hpp; a probe of its own would skip silently.
TEST(BuildIntegrity, EveryGpuTestGuardsThroughTheSharedHeader)
{
    const fs::path tests_dir = repoRoot() / "Test" / "commit" / "VulkanEngine";
    static const std::regex kGpuTest(R"(^TEST\((GoldenRender|Integration),\s*(\w+)\))");
    static const std::string kProbe = std::string("glfwVulkan") + "Supported(";
    static const std::string kCaptureQuery = std::string("supportsFrame") + "Capture()";
    std::vector<std::string> unguarded;
    std::vector<std::string> own_probes;
    std::vector<std::string> own_capture_checks;
    std::size_t checked = 0;
    std::error_code error;
    for (fs::directory_iterator it(tests_dir, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (path.extension() != ".cpp" && path.extension() != ".hpp") { continue; }
        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        const std::string name = path.filename().string();
        if (name != "GpuTestGuard.hpp" && std::any_of(lines->begin(), lines->end(), [](const std::string &line) {
                return line.find(kProbe) != std::string::npos;
            })) {
            own_probes.push_back(name);
        }
        for (std::size_t index = 0; name != "GpuTestGuard.hpp" && index < lines->size(); ++index) {
            const std::string &line = (*lines)[index];
            // A harness may define the query; asking it outside the header skips past KATAGLYPHIS_REQUIRE_GPU.
            if (line.find(kCaptureQuery) != std::string::npos
                && line.find("bool " + kCaptureQuery) == std::string::npos) {
                own_capture_checks.push_back(name + ":" + std::to_string(index + 1));
            }
        }
        for (std::size_t index = 0; index < lines->size(); ++index) {
            std::smatch match;
            if (!std::regex_search((*lines)[index], match, kGpuTest)) { continue; }
            ++checked;
            bool guarded = false;
            for (std::size_t body = index + 1; body < lines->size() && !(*lines)[body].starts_with("TEST("); ++body) {
                const std::string &line = (*lines)[body];
                if (line.find("SKIP_WITHOUT_GPU();") != std::string::npos
                    || line.find("SKIP_WITHOUT_HEADLESS_DEVICE(") != std::string::npos) {
                    guarded = true;
                    break;
                }
            }
            if (!guarded) { unguarded.push_back(match[1].str() + "." + match[2].str() + " in " + name); }
        }
    }

    EXPECT_GT(checked, 0U) << "found no TEST(GoldenRender|Integration, ...) under " << tests_dir.string();
    EXPECT_TRUE(unguarded.empty()) << unguarded.size()
                                   << " GPU test(s) guard through neither SKIP_WITHOUT_GPU() nor "
                                      "SKIP_WITHOUT_HEADLESS_DEVICE() from GpuTestGuard.hpp:"
                                   << joinViolations(unguarded);
    EXPECT_TRUE(own_probes.empty()) << "file(s) probe GLFW for Vulkan themselves, past KATAGLYPHIS_REQUIRE_GPU:"
                                    << joinViolations(own_probes);
    EXPECT_TRUE(own_capture_checks.empty()) << "line(s) ask a harness for frame capture themselves; "
                                               "SKIP_WITHOUT_FRAME_CAPTURE, or frameCaptureAvailable in a "
                                               "helper, fails them under KATAGLYPHIS_REQUIRE_GPU:"
                                            << joinViolations(own_capture_checks);
}

namespace {

// Whether the last line in [begin, end) naming KATAGLYPHIS_REQUIRE_GPU exports it as 1, unindented when `top_level`.
bool requires_gpu_before(const std::vector<std::string> &lines, std::size_t begin, std::size_t end, bool top_level)
{
    for (std::size_t index = std::min(end, lines.size()); index > begin; --index) {
        const std::string &line = lines[index - 1];
        const std::size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') { continue; }
        if (line.find("KATAGLYPHIS_REQUIRE_GPU") == std::string::npos) { continue; }
        return line.compare(start, std::string::npos, "export KATAGLYPHIS_REQUIRE_GPU=1") == 0
               && (!top_level || start == 0);
    }
    return false;
}

// The first line at or after `from` that is not a comment and contains `needle`.
std::optional<std::size_t>
  first_shell_line_with(const std::vector<std::string> &lines, std::size_t from, std::string_view needle)
{
    for (std::size_t index = from; index < lines.size(); ++index) {
        const std::size_t start = lines[index].find_first_not_of(" \t");
        if (start != std::string::npos && lines[index][start] != '#'
            && lines[index].find(needle) != std::string::npos) {
            return index;
        }
    }
    return std::nullopt;
}

}// namespace

// The GPU suites skip on a broken Xvfb unless the run exports KATAGLYPHIS_REQUIRE_GPU, and ctest counts a skip as a pass.
TEST(BuildIntegrity, LinuxGpuRunsRequireTheGpuBeforeTheirVirtualDisplay)
{
    const fs::path scripts_dir = repoRoot() / "scripts" / "linux";

    const fs::path ctest_path = scripts_dir / "run-ctest.sh";
    const auto ctest = readFileLines(ctest_path);
    ASSERT_TRUE(ctest.has_value()) << "could not open " << ctest_path.string();
    const auto branch = first_shell_line_with(*ctest, 0, R"(== "--virtual-display" ]])");
    ASSERT_TRUE(branch.has_value()) << ctest_path.string() << " has no --virtual-display branch";
    const auto ctest_display = first_shell_line_with(*ctest, *branch, "xvfb-run");
    ASSERT_TRUE(ctest_display.has_value()) << ctest_path.string() << "'s --virtual-display branch runs no xvfb-run";
    EXPECT_TRUE(requires_gpu_before(*ctest, *branch + 1, *ctest_display, false))
      << ctest_path.string() << "'s --virtual-display branch must export KATAGLYPHIS_REQUIRE_GPU=1 before its xvfb-run";

    const fs::path riscv_path = scripts_dir / "run-riscv64-tests.sh";
    const auto riscv = readFileLines(riscv_path);
    ASSERT_TRUE(riscv.has_value()) << "could not open " << riscv_path.string();
    const auto riscv_display = first_shell_line_with(*riscv, 0, "xvfb-run");
    ASSERT_TRUE(riscv_display.has_value()) << riscv_path.string() << " runs its GPU shard under no xvfb-run";
    // Unindented: the shard run is the script's top-level flow, which an export inside a branch would not reach.
    EXPECT_TRUE(requires_gpu_before(*riscv, 0, *riscv_display, true))
      << riscv_path.string() << " must export KATAGLYPHIS_REQUIRE_GPU=1 at top level before the GPU shard's xvfb-run";

    const fs::path cargo_path = scripts_dir / "run-cargo-tests.sh";
    const auto cargo = readFileLines(cargo_path);
    ASSERT_TRUE(cargo.has_value()) << "could not open " << cargo_path.string();
    const auto cargo_display = first_shell_line_with(*cargo, 0, "xvfb-run");
    ASSERT_TRUE(cargo_display.has_value()) << cargo_path.string() << " runs OxidANT's tests under no xvfb-run";
    EXPECT_NE((*cargo)[*cargo_display].find("CARGO_TEST_SH"), std::string::npos)
      << cargo_path.string() << "'s xvfb-run must run the hub's cargo_test.sh";
    EXPECT_TRUE(requires_gpu_before(*cargo, 0, *cargo_display, true))
      << cargo_path.string() << " must export KATAGLYPHIS_REQUIRE_GPU=1 at top level before its xvfb-run";
}

namespace {

// The shell command starting at `first`, its backslash-continued lines joined.
std::string shell_command_from(const std::vector<std::string> &lines, std::size_t first)
{
    std::string command;
    for (std::size_t index = first; index < lines.size(); ++index) {
        command += lines[index];
        command += '\n';
        if (lines[index].empty() || lines[index].back() != '\\') { break; }
    }
    return command;
}

bool names_both_gpu_suites(std::string_view text)
{ return text.find("GoldenRender") != std::string_view::npos && text.find("Integration") != std::string_view::npos; }

// `"${name[@]}"` for a line assigning the array `name`; empty for any other line.
std::string shell_array_expansion(const std::string &line)
{
    static const std::regex assignment(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)=\()");
    std::smatch match;
    if (!std::regex_search(line, match, assignment)) { return {}; }
    return "\"${" + match[1].str() + "[@]}\"";
}

}// namespace

// ctest counts a skipped test as passed, so each Linux GPU run hands both GPU suites to the hub's --fail-on-skipped.
TEST(BuildIntegrity, LinuxGpuRunsFailOnASkippedGpuTest)
{
    const fs::path scripts_dir = repoRoot() / "scripts" / "linux";

    const fs::path ctest_path = scripts_dir / "run-ctest.sh";
    const auto ctest = readFileLines(ctest_path);
    ASSERT_TRUE(ctest.has_value()) << "could not open " << ctest_path.string();
    const auto branch = first_shell_line_with(*ctest, 0, R"(== "--virtual-display" ]])");
    ASSERT_TRUE(branch.has_value()) << ctest_path.string() << " has no --virtual-display branch";
    const auto display = first_shell_line_with(*ctest, *branch, "xvfb-run");
    ASSERT_TRUE(display.has_value()) << ctest_path.string() << "'s --virtual-display branch runs no xvfb-run";
    const std::string display_command = shell_command_from(*ctest, *display);
    bool passes_flag =
      display_command.find("--fail-on-skipped") != std::string::npos && names_both_gpu_suites(display_command);
    if (const auto flag = first_shell_line_with(*ctest, *branch, "--fail-on-skipped");
      !passes_flag && flag.has_value() && *flag < *display) {
        const std::string expansion = shell_array_expansion((*ctest)[*flag]);
        passes_flag = names_both_gpu_suites((*ctest)[*flag]) && !expansion.empty()
                      && display_command.find(expansion) != std::string::npos;
    }
    EXPECT_TRUE(passes_flag) << ctest_path.string()
                             << "'s --virtual-display xvfb-run must pass --fail-on-skipped with a pattern naming "
                                "GoldenRender and Integration";

    const fs::path riscv_path = scripts_dir / "run-riscv64-tests.sh";
    const auto riscv = readFileLines(riscv_path);
    ASSERT_TRUE(riscv.has_value()) << "could not open " << riscv_path.string();
    static const std::regex direct_ctest(R"((^|[\s;&|(])ctest(\s|$))");
    std::vector<std::string> direct;
    for (std::size_t index = 0; index < riscv->size(); ++index) {
        const std::string &line = (*riscv)[index];
        const std::size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') { continue; }
        if (std::regex_search(line, direct_ctest)) { direct.push_back(std::to_string(index + 1) + ": " + line); }
    }
    EXPECT_TRUE(direct.empty()) << riscv_path.string()
                                << " runs ctest itself, past the hub's --fail-on-skipped:" << joinViolations(direct);
    const auto suites = first_shell_line_with(*riscv, 0, "CTEST_GPU_SUITES=");
    ASSERT_TRUE(suites.has_value()) << riscv_path.string() << " defines no CTEST_GPU_SUITES";
    EXPECT_TRUE(names_both_gpu_suites((*riscv)[*suites])) << riscv_path.string() << ": " << (*riscv)[*suites];
    const auto gpu_branch = first_shell_line_with(*riscv, 0, R"(== "--gpu-ctest" ]])");
    ASSERT_TRUE(gpu_branch.has_value()) << riscv_path.string() << " has no --gpu-ctest branch for the shard's ctest";
    const auto branch_end = std::find(riscv->begin() + static_cast<std::ptrdiff_t>(*gpu_branch), riscv->end(), "fi");
    ASSERT_NE(branch_end, riscv->end()) << riscv_path.string() << "'s --gpu-ctest branch has no unindented fi";
    const auto run = first_shell_line_with(*riscv, *gpu_branch, "ctest_run_main");
    ASSERT_TRUE(run.has_value() && *run < static_cast<std::size_t>(branch_end - riscv->begin()))
      << riscv_path.string() << "'s --gpu-ctest branch does not run the hub's ctest_run_main";
    EXPECT_NE((*riscv)[*run].find(R"(--fail-on-skipped "${CTEST_GPU_SUITES}")"), std::string::npos)
      << riscv_path.string() << "'s --gpu-ctest branch must pass --fail-on-skipped \"${CTEST_GPU_SUITES}\"";
    const auto shard_display =
      first_shell_line_with(*riscv, static_cast<std::size_t>(branch_end - riscv->begin()), "xvfb-run");
    ASSERT_TRUE(shard_display.has_value()) << riscv_path.string() << " runs its GPU shard under no xvfb-run";
    EXPECT_NE(
      shell_command_from(*riscv, *shard_display).find(R"(bash "${BASH_SOURCE[0]}" --gpu-ctest)"), std::string::npos)
      << riscv_path.string() << "'s GPU shard xvfb-run must re-run the script's --gpu-ctest branch";
}
