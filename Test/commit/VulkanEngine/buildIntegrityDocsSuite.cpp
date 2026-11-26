// Build integrity: the docs' counts, markers and citations match the suites and sources.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "BuildIntegrityScan.hpp"

using namespace Kataglyphis::TestSupport::BuildIntegrity;

namespace {

// Test names of `suite`, anchored like collect_defined_suites; file I/O only, so GPU suites list without a GPU.
std::vector<std::string> collect_suite_test_names(const fs::path &tests_dir, const std::string &suite)
{
    std::vector<std::string> names;
    const std::string macro_prefix = "TEST(" + suite + ",";
    std::error_code error;
    for (fs::recursive_directory_iterator it(tests_dir, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error) || it->path().extension() != ".cpp") { continue; }

        const auto lines = readFileLines(it->path());
        if (!lines) { continue; }
        for (const auto &line : *lines) {
            const std::size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos) { continue; }
            if (line.compare(start, macro_prefix.size(), macro_prefix) != 0) { continue; }

            const std::size_t name_start = start + macro_prefix.size();
            const std::size_t close_paren = line.find(')', name_start);
            if (close_paren == std::string::npos) { continue; }

            const std::size_t name_begin = line.find_first_not_of(" \t", name_start);
            const std::size_t name_end = line.find_last_not_of(" \t", close_paren - 1);
            if (name_begin == std::string::npos || name_begin > name_end) { continue; }

            names.push_back(line.substr(name_begin, name_end - name_begin + 1));
        }
    }
    return names;
}

// The five integers of docs/source/gpu-golden-testing.md's golden-counts marker line.
struct GoldenCountsMarker
{
    int defined = 0;
    int runnable = 0;
    int integration = 0;
    int total = 0;
    int excluded = 0;
};

std::optional<int> parse_marker_field(const std::string &line, const std::string &key)
{
    const std::size_t pos = line.find(key);
    if (pos == std::string::npos) { return std::nullopt; }

    const std::size_t digits_start = pos + key.size();
    std::size_t digits_end = digits_start;
    while (digits_end < line.size() && std::isdigit(static_cast<unsigned char>(line[digits_end]))) { ++digits_end; }
    if (digits_end == digits_start) { return std::nullopt; }

    return std::stoi(line.substr(digits_start, digits_end - digits_start));
}

// std::nullopt if the marker or any field is missing, so a deleted marker fails rather than passes.
std::optional<GoldenCountsMarker> parse_golden_counts_marker(const fs::path &doc_path)
{
    const auto lines = readFileLines(doc_path);
    if (!lines) { return std::nullopt; }

    for (const auto &line : *lines) {
        if (line.find("<!-- golden-counts:") == std::string::npos) { continue; }

        const auto defined_val = parse_marker_field(line, "defined=");
        const auto runnable_val = parse_marker_field(line, "runnable=");
        const auto integration_val = parse_marker_field(line, "integration=");
        const auto total_val = parse_marker_field(line, "total=");
        const auto excluded_val = parse_marker_field(line, "excluded=");
        if (!defined_val || !runnable_val || !integration_val || !total_val || !excluded_val) { return std::nullopt; }

        GoldenCountsMarker marker;
        marker.defined = *defined_val;
        marker.runnable = *runnable_val;
        marker.integration = *integration_val;
        marker.total = *total_val;
        marker.excluded = *excluded_val;
        return marker;
    }
    return std::nullopt;
}

// The doc's known-issue filter exclusions as (suite, name); only lines with `:-` count, and finding none must fail.
std::optional<std::vector<std::pair<std::string, std::string>>> parse_golden_test_exclusion_filter(
  const fs::path &doc_path)
{
    const auto lines = readFileLines(doc_path);
    if (!lines) { return std::nullopt; }

    static const std::string kFilterKey = "--gtest_filter='";
    for (const auto &line : *lines) {
        const std::size_t filter_start = line.find(kFilterKey);
        if (filter_start == std::string::npos) { continue; }

        const std::size_t value_start = filter_start + kFilterKey.size();
        const std::size_t value_end = line.find('\'', value_start);
        if (value_end == std::string::npos) { continue; }

        const std::string filter = line.substr(value_start, value_end - value_start);
        const std::size_t exclude_start = filter.find(":-");
        if (exclude_start == std::string::npos) { continue; }

        std::vector<std::pair<std::string, std::string>> excluded;
        std::size_t pos = exclude_start + 2;
        while (pos < filter.size()) {
            std::size_t next = filter.find(':', pos);
            if (next == std::string::npos) { next = filter.size(); }
            const std::string entry = filter.substr(pos, next - pos);
            const std::size_t dot = entry.find('.');
            if (dot != std::string::npos) { excluded.emplace_back(entry.substr(0, dot), entry.substr(dot + 1)); }
            pos = next + 1;
        }
        return excluded;
    }
    return std::vector<std::pair<std::string, std::string>>{};
}

// Names in docs/source/path-tracing.md's pt-goldens marker; nullopt if missing, malformed or empty, so deletion fails.
std::optional<std::vector<std::string>> parse_pt_goldens_marker(const fs::path &doc_path)
{
    const auto lines = readFileLines(doc_path);
    if (!lines) { return std::nullopt; }

    static const std::string kMarkerKey = "<!-- pt-goldens:";
    for (const auto &line : *lines) {
        const std::size_t marker_start = line.find(kMarkerKey);
        if (marker_start == std::string::npos) { continue; }

        const std::size_t value_start = marker_start + kMarkerKey.size();
        const std::size_t value_end = line.find("-->", value_start);
        if (value_end == std::string::npos) { return std::nullopt; }

        const std::string list = line.substr(value_start, value_end - value_start);
        std::vector<std::string> names;
        std::size_t pos = 0;
        while (pos <= list.size()) {
            std::size_t next = list.find(',', pos);
            if (next == std::string::npos) { next = list.size(); }
            const std::string entry = list.substr(pos, next - pos);
            const std::size_t begin = entry.find_first_not_of(" \t");
            const std::size_t end = entry.find_last_not_of(" \t");
            if (begin != std::string::npos && begin <= end) { names.push_back(entry.substr(begin, end - begin + 1)); }
            if (next == list.size()) { break; }
            pos = next + 1;
        }
        if (names.empty()) { return std::nullopt; }
        return names;
    }
    return std::nullopt;
}

}// namespace

// Counts typed into the doc's prose outside the marker: the second copy that drifts while the marker stays right.
std::vector<std::string> find_bare_golden_count_copies(const fs::path &doc_path)
{
    std::vector<std::string> violations;
    const auto lines = readFileLines(doc_path);
    if (!lines) { return violations; }

    static const std::array<std::string, 3> kKeywords = { "runnable", "defined", "`Integration` tests" };

    for (std::size_t line_no = 0; line_no < lines->size(); ++line_no) {
        const std::string &line = (*lines)[line_no];
        if (line.find("<!-- golden-counts:") != std::string::npos) { continue; }

        for (const auto &keyword : kKeywords) {
            const std::size_t pos = line.find(keyword);
            if (pos == std::string::npos) { continue; }

            std::size_t digit_end = pos;
            while (digit_end > 0 && line[digit_end - 1] == ' ') { --digit_end; }
            std::size_t digit_start = digit_end;
            while (digit_start > 0 && std::isdigit(static_cast<unsigned char>(line[digit_start - 1]))) {
                --digit_start;
            }

            if (digit_start != digit_end) {
                violations.push_back("line " + std::to_string(line_no + 1) + ": \"" + line + "\"");
            }
        }
    }
    return violations;
}

TEST(BuildIntegrity, GoldenTestCountsInDocsMatchTheSuite)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path doc_path = repo_root / "docs" / "source" / "gpu-golden-testing.md";
    if (!sourceExists(doc_path)) {
        GTEST_SKIP() << "could not open " << doc_path.string() << " - not running from the repo root?";
    }

    const auto marker = parse_golden_counts_marker(doc_path);
    ASSERT_TRUE(marker.has_value())
      << doc_path.string()
      << " is missing its '<!-- golden-counts: defined=N runnable=N integration=N total=N excluded=N -->' marker "
         "line, or one of its five fields - a deleted marker must fail this test, not silently pass";

    const fs::path tests_dir = repo_root / "Test" / "commit" / "VulkanEngine";
    const std::vector<std::string> golden_tests = collect_suite_test_names(tests_dir, "GoldenRender");
    const std::vector<std::string> integration_tests = collect_suite_test_names(tests_dir, "Integration");

    const int counted_defined = static_cast<int>(golden_tests.size());
    const int counted_runnable =
      static_cast<int>(std::count_if(golden_tests.begin(), golden_tests.end(), [](const std::string &name) {
          return !name.starts_with("DISABLED_");
      }));
    const int counted_integration = static_cast<int>(integration_tests.size());

    EXPECT_EQ(marker->defined, counted_defined)
      << doc_path.string() << "'s golden-counts marker says defined=" << marker->defined << " but "
      << tests_dir.string() << " has " << counted_defined << " TEST(GoldenRender, ...) definitions";
    EXPECT_EQ(marker->runnable, counted_runnable)
      << doc_path.string() << "'s golden-counts marker says runnable=" << marker->runnable << " but "
      << counted_runnable << " of " << counted_defined
      << " TEST(GoldenRender, ...) definitions do not start with DISABLED_";
    EXPECT_EQ(marker->integration, counted_integration)
      << doc_path.string() << "'s golden-counts marker says integration=" << marker->integration << " but "
      << tests_dir.string() << " has " << counted_integration << " TEST(Integration, ...) definitions";
    EXPECT_EQ(marker->runnable + marker->integration, marker->total)
      << doc_path.string() << "'s golden-counts marker is internally inconsistent: runnable(" << marker->runnable
      << ") + integration(" << marker->integration << ") != total(" << marker->total << ")";

    const auto exclusion_filter = parse_golden_test_exclusion_filter(doc_path);
    ASSERT_TRUE(exclusion_filter.has_value())
      << doc_path.string() << " is missing its '--gtest_filter=' line in the \"Known issue\" section";

    const int counted_excluded = static_cast<int>(exclusion_filter->size());
    EXPECT_EQ(marker->excluded, counted_excluded)
      << doc_path.string() << "'s golden-counts marker says excluded=" << marker->excluded
      << " but the --gtest_filter= line's ':-'-prefixed section names " << counted_excluded << " test(s)";

    for (const auto &[suite, name] : *exclusion_filter) {
        const std::vector<std::string> &suite_tests = suite == "GoldenRender" ? golden_tests : integration_tests;
        EXPECT_TRUE(std::find(suite_tests.begin(), suite_tests.end(), name) != suite_tests.end())
          << doc_path.string() << "'s --gtest_filter= excludes " << suite << "." << name << " but no such TEST("
          << suite << ", " << name << ") exists in " << tests_dir.string();
    }

    const auto bare_copies = find_bare_golden_count_copies(doc_path);
    EXPECT_TRUE(bare_copies.empty())
      << doc_path.string()
      << " hand-types a count next to 'runnable'/'defined'/'`Integration` tests' outside the golden-counts marker "
         "- the marker is the single source of truth, a second copy just drifts out of sync with it:"
      << joinViolations(bare_copies);
}

// Pins docs/source/path-tracing.md's pt-goldens marker to the PathTracing and Raytraced goldens the suite defines.
TEST(BuildIntegrity, PathTracingDocMatchesTheGoldenSuite)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path doc_path = repo_root / "docs" / "source" / "path-tracing.md";
    if (!sourceExists(doc_path)) {
        GTEST_SKIP() << "could not open " << doc_path.string() << " - not running from the repo root?";
    }

    const auto marker_names = parse_pt_goldens_marker(doc_path);
    ASSERT_TRUE(marker_names.has_value())
      << doc_path.string()
      << " is missing its '<!-- pt-goldens: name1, name2, ... -->' marker line, or it lists zero names - a "
         "deleted marker must fail this test, not silently pass";

    const fs::path tests_dir = repo_root / "Test" / "commit" / "VulkanEngine";
    const std::vector<std::string> golden_tests = collect_suite_test_names(tests_dir, "GoldenRender");

    std::vector<std::string> pt_tests;
    for (const auto &name : golden_tests) {
        if (name.starts_with("PathTracing") || name.starts_with("Raytraced")) { pt_tests.push_back(name); }
    }

    std::vector<std::string> missing_from_doc;
    for (const auto &name : pt_tests) {
        if (std::find(marker_names->begin(), marker_names->end(), name) == marker_names->end()) {
            missing_from_doc.push_back(name);
        }
    }
    std::vector<std::string> extra_in_doc;
    for (const auto &name : *marker_names) {
        if (std::find(pt_tests.begin(), pt_tests.end(), name) == pt_tests.end()) { extra_in_doc.push_back(name); }
    }

    const auto joined = [](const std::vector<std::string> &names) -> std::string {
        std::string result;
        for (const auto &name : names) { result += name + " "; }
        return result;
    };

    EXPECT_TRUE(missing_from_doc.empty() && extra_in_doc.empty())
      << doc_path.string() << "'s '<!-- pt-goldens: ... -->' marker is out of sync with " << tests_dir.string()
      << "'s TEST(GoldenRender, PathTracing...)/TEST(GoldenRender, Raytraced...) definitions - missing from doc: ["
      << joined(missing_from_doc) << "], extra in doc: [" << joined(extra_in_doc) << "]";

    const fs::path pathtracing_cpp_path = repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "PathTracing.cpp";
    const auto cpp_content_opt = readFileText(pathtracing_cpp_path);
    ASSERT_TRUE(cpp_content_opt.has_value()) << "could not open " << pathtracing_cpp_path.string();
    const std::string &cpp_content = *cpp_content_opt;

    const auto doc_content_opt = readFileText(doc_path);
    ASSERT_TRUE(doc_content_opt.has_value()) << "could not open " << doc_path.string();
    const std::string &doc_content = *doc_content_opt;

    const bool shader_ships_furnace = cpp_content.find("KATAGLYPHIS_PT_FURNACE") != std::string::npos;
    const bool doc_still_wants_toggle = doc_content.find("wants a uniform-environment toggle") != std::string::npos;
    EXPECT_FALSE(shader_ships_furnace && doc_still_wants_toggle)
      << doc_path.string()
      << " still asks for a uniform-environment furnace toggle (\"wants a uniform-environment toggle\"), but "
      << pathtracing_cpp_path.string()
      << " already ships it (\"KATAGLYPHIS_PT_FURNACE\") - the doc is asking for shipped work.";
}

// std::nullopt if the max-texture-count marker or its value is missing, so a deleted marker fails.
std::optional<int> parse_max_texture_count_marker(const fs::path &doc_path)
{
    const auto lines = readFileLines(doc_path);
    if (!lines) { return std::nullopt; }

    static const std::regex kMarkerPattern(R"(<!--\s*max-texture-count:\s*(\d+)\s*-->)");

    for (const auto &line : *lines) {
        std::smatch match;
        if (std::regex_search(line, match, kMarkerPattern)) { return std::stoi(match[1].str()); }
    }
    return std::nullopt;
}

// Plain file I/O, not an include: the point is to catch the constant changing under the doc.
std::optional<int> parse_max_texture_count_header(const fs::path &header_path)
{
    const auto lines = readFileLines(header_path);
    if (!lines) { return std::nullopt; }

    static const std::regex kMaxTextureCountPattern(R"(const\s+int\s+MAX_TEXTURE_COUNT\s*=\s*(\d+)\s*;)");

    for (const auto &line : *lines) {
        std::smatch match;
        if (std::regex_search(line, match, kMaxTextureCountPattern)) { return std::stoi(match[1].str()); }
    }
    return std::nullopt;
}

// Pins docs/source/model-loading.md's max-texture-count marker to the header constant.
TEST(BuildIntegrity, MaxTextureCountInDocsMatchesTheHeader)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path doc_path = repo_root / "docs" / "source" / "model-loading.md";
    if (!sourceExists(doc_path)) {
        GTEST_SKIP() << "could not open " << doc_path.string() << " - not running from the repo root?";
    }

    const auto doc_value = parse_max_texture_count_marker(doc_path);
    ASSERT_TRUE(doc_value.has_value())
      << doc_path.string()
      << " is missing its '<!-- max-texture-count: N -->' marker line - a deleted marker must "
         "fail this test, not silently pass";

    const fs::path header_path = slangRoot() / "common" / "shared_constants.slang";
    const auto header_value = parse_max_texture_count_header(header_path);
    ASSERT_TRUE(header_value.has_value())
      << header_path.string() << " does not define 'static const int MAX_TEXTURE_COUNT = <N>;'";

    EXPECT_EQ(*doc_value, *header_value) << doc_path.string() << "'s max-texture-count marker says " << *doc_value
                                         << " but " << header_path.string()
                                         << " defines MAX_TEXTURE_COUNT = " << *header_value;
}

// Cite symbols, not line numbers, which silently point at unrelated code once a function moves; bare colons rot too.
TEST(BuildIntegrity, SourceAndDocsCiteSymbolsNotLineNumbers)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const std::regex kFileLinePattern(R"([A-Za-z_/.-]+\.(cpp|ixx|hpp|slang|wgsl|rs):[0-9]+)");
    static const std::regex kBareLinePattern(R"((^|[\s(`\[]):[0-9]+(-[0-9]+)?([^0-9]|$))");

    std::vector<std::string> violations;
    auto scan_file = [&](const fs::path &path) {
        const auto content = readFileText(path);
        if (!content.has_value()) { return; }
        const std::string relative = fs::relative(path, repo_root).generic_string();
        auto begin = std::sregex_iterator(content->begin(), content->end(), kFileLinePattern);
        for (auto it = begin; it != std::sregex_iterator(); ++it) { violations.push_back(relative + ": " + it->str()); }
        auto bare_begin = std::sregex_iterator(content->begin(), content->end(), kBareLinePattern);
        for (auto it = bare_begin; it != std::sregex_iterator(); ++it) {
            violations.push_back(relative + ": " + it->str());
        }
    };

    scan_file(repo_root / "docs" / "source" / "model-loading.md");
    scan_file(repo_root / "docs" / "source" / "clouds.md");

    static constexpr std::array<const char *, 3> kSrcExtensions{ ".cpp", ".hpp", ".ixx" };
    std::error_code error;
    for (fs::recursive_directory_iterator it(repo_root / "Src", error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error)) { continue; }
        const std::string extension = it->path().extension().string();
        if (std::find(kSrcExtensions.begin(), kSrcExtensions.end(), extension) == kSrcExtensions.end()) { continue; }
        scan_file(it->path());
    }

    for (fs::recursive_directory_iterator it(repo_root / "Resources" / "ShadersSlang", error), end; it != end;
      it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error) || it->path().extension() != ".slang") { continue; }
        scan_file(it->path());
    }

    for (fs::recursive_directory_iterator it(repo_root / "Test", error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error)) { continue; }
        const std::string extension = it->path().extension().string();
        if (std::find(kSrcExtensions.begin(), kSrcExtensions.end(), extension) == kSrcExtensions.end()) { continue; }
        scan_file(it->path());
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " file:line location(s), including the same-file `:NNN` shorthand, across Src/, "
         "Resources/ShadersSlang/, Test/ and docs/source/model-loading.md, which rot within days - cite the "
         "function or member name instead:"
      << joinViolations(violations);
}

// std::nullopt if the format-drift-denominator marker or its value is missing, so a deleted marker fails.
std::optional<int> parse_format_drift_denominator_marker(const fs::path &doc_path)
{
    const auto lines = readFileLines(doc_path);
    if (!lines) { return std::nullopt; }

    static const std::regex kMarkerPattern(R"(<!--\s*format-drift-denominator:\s*(\d+)\s*-->)");

    for (const auto &line : *lines) {
        std::smatch match;
        if (std::regex_search(line, match, kMarkerPattern)) { return std::stoi(match[1].str()); }
    }
    return std::nullopt;
}

// Counts what Get-ProjectCppFiles tracks, skipping build* directories like its git-less fallback.
std::size_t count_cpp_sources(const fs::path &root)
{
    static const std::set<std::string> kCppExtensions = { ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".ixx" };

    std::size_t count = 0;
    std::error_code error;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
      it != end;
      it.increment(error)) {
        if (error) { break; }
        if (it->is_directory(error) && it->path().filename().string().starts_with("build")) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(error) || !kCppExtensions.contains(it->path().extension().string())) { continue; }
        ++count;
    }
    return count;
}

// Pins only the drift figure's denominator to a live count; the numerator needs clang-format and a human.
TEST(BuildIntegrity, FormatDriftDenominatorMatchesTheTrackedSourceCount)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path doc_path = repo_root / "docs" / "source" / "code-quality.md";
    if (!sourceExists(doc_path)) {
        GTEST_SKIP() << "could not open " << doc_path.string() << " - not running from the repo root?";
    }

    const auto doc_value = parse_format_drift_denominator_marker(doc_path);
    ASSERT_TRUE(doc_value.has_value())
      << doc_path.string()
      << " is missing its '<!-- format-drift-denominator: N -->' marker line - a deleted marker must fail this "
         "test, not silently pass";

    const std::size_t actual_count = count_cpp_sources(repo_root / "Src") + count_cpp_sources(repo_root / "Test");

    EXPECT_EQ(static_cast<std::size_t>(*doc_value), actual_count)
      << doc_path.string() << "'s format-drift-denominator marker says " << *doc_value << " but Src/ + Test/ "
      << "currently contain " << actual_count
      << " tracked C/C++ sources - re-run the clang-format drift measurement in docs/source/code-quality.md's \"Known "
         "state\" section and update both the marker and the X-of-Y prose (this test only pins the "
         "denominator, not the deviating count).";
}

// The log must not ask for the swapchain barrier removal the source has already shipped.
TEST(BuildIntegrity, RendererImprovementLogDoesNotAskForShippedWork)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path doc_path = repo_root / "docs" / "source" / "cpp-renderer-improvements.md";
    const fs::path renderer_path = repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "VulkanRenderer.cpp";

    const auto doc_content_opt = readFileText(doc_path);
    ASSERT_TRUE(doc_content_opt.has_value()) << "could not open " << doc_path.string();
    const std::string &doc_content = *doc_content_opt;

    const auto renderer_content_opt = readFileText(renderer_path);
    ASSERT_TRUE(renderer_content_opt.has_value()) << "could not open " << renderer_path.string();
    const std::string &renderer_content = *renderer_content_opt;

    const bool renderer_records_the_removal = renderer_content.find("used to sit here") != std::string::npos;
    const bool doc_still_asks_for_the_removal =
      doc_content.find("remove only after a sync-validation") != std::string::npos;

    EXPECT_FALSE(renderer_records_the_removal && doc_still_asks_for_the_removal)
      << doc_path.string()
      << " still asks for the same-layout swapchain barrier to be removed "
         "(\"remove only after a sync-validation\"), but "
      << renderer_path.string()
      << " already records the removal (\"used to sit here\") - the doc is asking for shipped work.";

    const std::string in_progress_marker = "## In progress";
    const auto in_progress_pos = doc_content.find(in_progress_marker);
    ASSERT_NE(in_progress_pos, std::string::npos) << doc_path.string() << " is missing its \"## In progress\" section";
    const auto queued_pos = doc_content.find("## Queued", in_progress_pos);
    ASSERT_NE(queued_pos, std::string::npos) << doc_path.string() << " is missing its \"## Queued\" section";
    const std::string in_progress_section = doc_content.substr(in_progress_pos, queued_pos - in_progress_pos);

    EXPECT_EQ(in_progress_section.find("sync-validated barrier removal"), std::string::npos)
      << doc_path.string()
      << "'s \"## In progress\" section still lists \"sync-validated barrier removal\" as "
         "remaining queue, but "
      << renderer_path.string() << " already records the removal (\"used to sit here\").";
}

// The docs must name this repository, not its old slug: nothing else gates prose after a rename.
TEST(BuildIntegrity, DocsNameThisRepository)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const std::string kStaleSlug = "Kataglyphis-Renderer";

    std::vector<fs::path> candidates = { repo_root / "README.md" };

    const fs::path docs_source = repo_root / "docs" / "source";
    ASSERT_TRUE(sourceExists(docs_source)) << "missing " << docs_source.string();

    std::error_code error;
    for (fs::recursive_directory_iterator it(docs_source, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        const auto extension = path.extension();
        if (extension != ".md" && extension != ".rst" && path.filename() != "conf.py") { continue; }
        candidates.push_back(path);
    }

    std::vector<std::string> violations;
    for (const fs::path &path : candidates) {
        const auto text = readFileText(path);
        if (!text) { continue; }
        if (text->find(kStaleSlug) != std::string::npos) {
            violations.push_back(fs::relative(path, repo_root).generic_string());
        }
    }

    EXPECT_TRUE(violations.empty()) << violations.size() << " doc file(s) still reference the old repository slug \""
                                    << kStaleSlug << "\":" << joinViolations(violations);
}

// Parses docs/source/cpp-renderer-improvements.md's `<!-- commit-suite-test-count: N -->` marker.
std::optional<int> parse_commit_suite_test_count_marker(const fs::path &doc_path)
{
    const auto lines = readFileLines(doc_path);
    if (!lines) { return std::nullopt; }

    static const std::regex kMarkerPattern(R"(<!--\s*commit-suite-test-count:\s*(\d+)\s*-->)");

    for (const auto &line : *lines) {
        std::smatch match;
        if (std::regex_search(line, match, kMarkerPattern)) { return std::stoi(match[1].str()); }
    }
    return std::nullopt;
}

// Counts TEST, TEST_F and TEST_P definition lines across the commit suite's .cpp files.
int count_commit_suite_tests(const fs::path &tests_dir)
{
    static const std::regex kTestPattern(R"(^(TEST|TEST_F|TEST_P)\()");

    int count = 0;
    std::error_code error;
    for (fs::recursive_directory_iterator it(tests_dir, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error) || it->path().extension() != ".cpp") { continue; }
        const auto lines = readFileLines(it->path());
        if (!lines) { continue; }
        for (const auto &line : *lines) {
            if (std::regex_search(line, kTestPattern)) { ++count; }
        }
    }
    return count;
}

// The log's quoted commit-suite test count must be current: churny by design, and a missing marker fails.
TEST(BuildIntegrity, ImprovementLogQuotesTheCurrentCommitSuiteTestCount)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path doc_path = repo_root / "docs" / "source" / "cpp-renderer-improvements.md";
    const auto marker_value = parse_commit_suite_test_count_marker(doc_path);
    ASSERT_TRUE(marker_value.has_value())
      << doc_path.string()
      << " is missing its '<!-- commit-suite-test-count: N -->' marker line - this is a one-line update, add it "
         "back with the current count from Test/commit/VulkanEngine/.";

    const fs::path tests_dir = repo_root / "Test" / "commit" / "VulkanEngine";
    const int actual_count = count_commit_suite_tests(tests_dir);

    EXPECT_EQ(*marker_value, actual_count)
      << doc_path.string() << "'s commit-suite-test-count marker says " << *marker_value << " but "
      << tests_dir.string() << " actually defines " << actual_count
      << " TEST/TEST_F/TEST_P case(s) - this is a one-line update to the marker in " << doc_path.string() << ".";
}
