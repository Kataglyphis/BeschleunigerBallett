// Build integrity: CI and the runners name every fuzz target and benchmark, and pass the exit code on.

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "BuildIntegrityScan.hpp"

using namespace Kataglyphis::TestSupport::BuildIntegrity;

namespace {

// Fuzz targets declared via kataglyphis_add_fuzz_test(<name> ...); the definition has a space there and never matches.
std::vector<std::string> parse_declared_fuzz_targets(const fs::path &cmake_path)
{
    std::vector<std::string> targets;
    const auto lines = readFileLines(cmake_path);
    if (!lines) { return targets; }

    static const std::string kMacro = "kataglyphis_add_fuzz_test(";
    for (const auto &line : *lines) {
        const std::size_t pos = line.find(kMacro);
        if (pos == std::string::npos) { continue; }

        const std::size_t name_start = pos + kMacro.size();
        std::size_t name_end = name_start;
        while (name_end < line.size() && is_identifier_char(line[name_end])) { ++name_end; }
        if (name_end == name_start) { continue; }
        targets.push_back(line.substr(name_start, name_end - name_start));
    }
    return targets;
}

// Benchmark names, as "<name>/<n>" per chained ->Arg(<n>) like Google Benchmark's run names; zero found fails the gate.
std::vector<std::string> parse_declared_perf_benchmarks(const fs::path &source_path)
{
    std::vector<std::string> names;
    const auto lines = readFileLines(source_path);
    if (!lines) { return names; }

    static const std::string kMacro = "BENCHMARK(";
    for (const auto &line : *lines) {
        const std::size_t pos = line.find(kMacro);
        if (pos == std::string::npos) { continue; }

        const std::size_t name_start = pos + kMacro.size();
        std::size_t name_end = name_start;
        while (name_end < line.size() && is_identifier_char(line[name_end])) { ++name_end; }
        if (name_end == name_start) { continue; }
        const std::string benchmark_name = line.substr(name_start, name_end - name_start);

        std::vector<std::string> args;
        static const std::string kArgMacro = "->Arg(";
        std::size_t arg_pos = name_end;
        while ((arg_pos = line.find(kArgMacro, arg_pos)) != std::string::npos) {
            const std::size_t digits_start = arg_pos + kArgMacro.size();
            std::size_t digits_end = digits_start;
            while (digits_end < line.size() && std::isdigit(static_cast<unsigned char>(line[digits_end])) != 0) {
                ++digits_end;
            }
            if (digits_end > digits_start) { args.push_back(line.substr(digits_start, digits_end - digits_start)); }
            arg_pos = digits_end;
        }

        if (args.empty()) {
            names.push_back(benchmark_name);
        } else {
            for (const auto &arg : args) { names.push_back(benchmark_name + "/" + arg); }
        }
    }
    return names;
}

// benchmarks[].name from a Google Benchmark JSON baseline; no-throw parse, since exceptions are disabled.
std::optional<std::vector<std::string>> parse_perf_baseline_names(const fs::path &baseline_path)
{
    std::ifstream file(baseline_path);
    if (!file) { return std::nullopt; }

    nlohmann::json doc = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) { return std::nullopt; }

    const auto benchmarks_it = doc.find("benchmarks");
    if (benchmarks_it == doc.end() || !benchmarks_it->is_array()) { return std::nullopt; }

    std::vector<std::string> names;
    for (const auto &entry : *benchmarks_it) {
        if (!entry.is_object()) { continue; }
        const auto name_it = entry.find("name");
        if (name_it == entry.end() || !name_it->is_string()) { continue; }
        names.push_back(name_it->get<std::string>());
    }
    return names;
}

// Fuzz targets of Invoke-WindowsLane.ps1's seed loop, anchored so no other loop matches; empty means no anchor: fail.
std::optional<std::vector<std::string>> parse_ci_fuzz_targets(const fs::path &lane_path)
{
    const auto lines = readFileLines(lane_path);
    if (!lines) { return std::nullopt; }

    // Escaped when a host shell would expand $t, plain when the command arrives via env; accept both.
    static const std::array<std::string, 2> kAnchors = { "foreach (`$t in @(", "foreach ($t in @(" };
    static const std::string kCloser = "))";

    for (const auto &line : *lines) {
        std::size_t anchor_pos = std::string::npos;
        std::size_t anchor_size = 0;
        for (const auto &anchor : kAnchors) {
            const std::size_t pos = line.find(anchor);
            if (pos != std::string::npos) {
                anchor_pos = pos;
                anchor_size = anchor.size();
                break;
            }
        }
        if (anchor_pos == std::string::npos) { continue; }

        const std::size_t list_start = anchor_pos + anchor_size;
        const std::size_t closer_pos = line.find(kCloser, list_start);
        if (closer_pos == std::string::npos) { break; }

        const std::string list = line.substr(list_start, closer_pos - list_start);
        std::vector<std::string> targets;
        std::size_t pos = 0;
        while (pos < list.size()) {
            const std::size_t open_quote = list.find('\'', pos);
            if (open_quote == std::string::npos) { break; }
            const std::size_t close_quote = list.find('\'', open_quote + 1);
            if (close_quote == std::string::npos) { break; }
            targets.push_back(list.substr(open_quote + 1, close_quote - open_quote - 1));
            pos = close_quote + 1;
        }
        return targets;
    }
    return std::vector<std::string>{};
}

// Fuzz targets of reusable-linux.yml's "for t in ...; do" loop, anchored likewise; empty means no anchor: fail.
std::optional<std::vector<std::string>> parse_linux_ci_fuzz_targets(const fs::path &workflow_path)
{
    const auto lines = readFileLines(workflow_path);
    if (!lines) { return std::nullopt; }

    static const std::string kAnchor = "for t in ";
    static const std::string kCloser = "; do";

    for (const auto &line : *lines) {
        const std::size_t anchor_pos = line.find(kAnchor);
        if (anchor_pos == std::string::npos) { continue; }

        const std::size_t list_start = anchor_pos + kAnchor.size();
        const std::size_t closer_pos = line.find(kCloser, list_start);
        if (closer_pos == std::string::npos) { break; }

        const std::string list = line.substr(list_start, closer_pos - list_start);
        std::vector<std::string> targets;
        std::istringstream iss(list);
        std::string token;
        while (iss >> token) { targets.push_back(token); }
        return targets;
    }
    return std::vector<std::string>{};
}

// Fuzz executables of Invoke-ClangClDebug.ps1's loop without ".exe", to compare with the declared targets.
std::optional<std::vector<std::string>> parse_local_runner_fuzz_targets(const fs::path &script_path)
{
    const auto lines = readFileLines(script_path);
    if (!lines) { return std::nullopt; }

    static const std::string kAnchor = "foreach ($fuzzExecutable in @(";
    static const std::string kCloser = "))";
    static const std::string kExeSuffix = ".exe";

    for (const auto &line : *lines) {
        const std::size_t anchor_pos = line.find(kAnchor);
        if (anchor_pos == std::string::npos) { continue; }

        const std::size_t list_start = anchor_pos + kAnchor.size();
        const std::size_t closer_pos = line.find(kCloser, list_start);
        if (closer_pos == std::string::npos) { break; }

        const std::string list = line.substr(list_start, closer_pos - list_start);
        std::vector<std::string> targets;
        std::size_t pos = 0;
        while (pos < list.size()) {
            const std::size_t open_quote = list.find('\'', pos);
            if (open_quote == std::string::npos) { break; }
            const std::size_t close_quote = list.find('\'', open_quote + 1);
            if (close_quote == std::string::npos) { break; }
            std::string entry = list.substr(open_quote + 1, close_quote - open_quote - 1);
            if (entry.size() > kExeSuffix.size()
                && entry.compare(entry.size() - kExeSuffix.size(), kExeSuffix.size(), kExeSuffix) == 0) {
                entry.erase(entry.size() - kExeSuffix.size());
            }
            targets.push_back(entry);
            pos = close_quote + 1;
        }
        return targets;
    }
    return std::vector<std::string>{};
}

}// namespace

// Fuzz targets have no negative filter, so one missing from the Windows lane's array silently never runs.
TEST(BuildIntegrity, EveryFuzzTargetIsInTheWindowsCiFuzzList)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const std::vector<std::string> declared_targets =
      parse_declared_fuzz_targets(repo_root / "Test" / "fuzz" / "CMakeLists.txt");
    ASSERT_FALSE(declared_targets.empty())
      << "parsed zero kataglyphis_add_fuzz_test(...) declarations out of Test/fuzz/CMakeLists.txt - the "
         "anchor text ('kataglyphis_add_fuzz_test(') may have changed";
    const std::set<std::string> declared_set(declared_targets.begin(), declared_targets.end());

    const fs::path lane_path = repo_root / "scripts" / "windows" / "Invoke-WindowsLane.ps1";
    const auto ci_targets_opt = parse_ci_fuzz_targets(lane_path);
    if (!ci_targets_opt.has_value()) {
        GTEST_SKIP() << "could not open " << lane_path.string() << " - not running from the repo root?";
    }
    const std::vector<std::string> &ci_targets = *ci_targets_opt;
    ASSERT_FALSE(ci_targets.empty()) << "parsed zero fuzz targets out of the foreach array in " << lane_path.string()
                                     << R"( - the anchor text ('foreach (`$t in @(' / '))') may have changed)";
    const std::set<std::string> ci_set(ci_targets.begin(), ci_targets.end());

    // For a future smoke-only target that should not gate CI; empty, since every target runs today.
    const std::set<std::string> excluded_from_ci;

    std::vector<std::string> missing_from_ci;
    for (const auto &target : declared_targets) {
        if (ci_set.contains(target) || excluded_from_ci.contains(target)) { continue; }
        missing_from_ci.push_back(target);
    }
    EXPECT_TRUE(missing_from_ci.empty())
      << missing_from_ci.size()
      << " fuzz target(s) declared in Test/fuzz/CMakeLists.txt are neither in Invoke-WindowsLane.ps1's "
         "fuzz-seed foreach array nor in the smoke-target exclusion list, so they silently do not run in CI: "
      << joinViolations(missing_from_ci);

    std::vector<std::string> dead_ci_entries;
    for (const auto &target : ci_targets) {
        if (!declared_set.contains(target)) { dead_ci_entries.push_back(target); }
    }
    EXPECT_TRUE(dead_ci_entries.empty())
      << dead_ci_entries.size()
      << " entry/entries in Invoke-WindowsLane.ps1's fuzz-seed foreach array do not correspond to any target declared "
         "in Test/fuzz/CMakeLists.txt (renamed or deleted?): "
      << joinViolations(dead_ci_entries);
}

// Every declared fuzz target must run in the Linux workflow, Windows lane and local runner, or it goes unexercised.
TEST(BuildIntegrity, EveryRegisteredFuzzTargetRunsInCi)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const std::vector<std::string> declared_targets =
      parse_declared_fuzz_targets(repo_root / "Test" / "fuzz" / "CMakeLists.txt");
    ASSERT_FALSE(declared_targets.empty())
      << "parsed zero kataglyphis_add_fuzz_test(...) declarations out of Test/fuzz/CMakeLists.txt - the "
         "anchor text ('kataglyphis_add_fuzz_test(') may have changed";
    const std::set<std::string> declared_set(declared_targets.begin(), declared_targets.end());

    const fs::path linux_workflow_path = repo_root / ".github" / "workflows" / "reusable-linux.yml";
    const fs::path windows_lane_path = repo_root / "scripts" / "windows" / "Invoke-WindowsLane.ps1";
    const fs::path local_runner_path = repo_root / "scripts" / "windows" / "Invoke-ClangClDebug.ps1";

    const auto linux_targets_opt = parse_linux_ci_fuzz_targets(linux_workflow_path);
    if (!linux_targets_opt.has_value()) {
        GTEST_SKIP() << "could not open " << linux_workflow_path.string() << " - not running from the repo root?";
    }
    const auto windows_targets_opt = parse_ci_fuzz_targets(windows_lane_path);
    if (!windows_targets_opt.has_value()) {
        GTEST_SKIP() << "could not open " << windows_lane_path.string() << " - not running from the repo root?";
    }
    const auto local_targets_opt = parse_local_runner_fuzz_targets(local_runner_path);
    if (!local_targets_opt.has_value()) {
        GTEST_SKIP() << "could not open " << local_runner_path.string() << " - not running from the repo root?";
    }
    ASSERT_FALSE(linux_targets_opt->empty())
      << "parsed zero fuzz targets out of the for-loop in " << linux_workflow_path.string()
      << " - the anchor text ('for t in ' / '; do') may have changed";
    ASSERT_FALSE(windows_targets_opt->empty())
      << "parsed zero fuzz targets out of the foreach array in " << windows_lane_path.string()
      << R"( - the anchor text ('foreach (`$t in @(' / '))') may have changed)";
    ASSERT_FALSE(local_targets_opt->empty())
      << "parsed zero fuzz targets out of the foreach array in " << local_runner_path.string()
      << R"( - the anchor text ('foreach ($fuzzExecutable in @(' / '))') may have changed)";

    const std::set<std::string> linux_set(linux_targets_opt->begin(), linux_targets_opt->end());
    const std::set<std::string> windows_set(windows_targets_opt->begin(), windows_targets_opt->end());
    const std::set<std::string> local_set(local_targets_opt->begin(), local_targets_opt->end());

    // A target may skip a lane only if listed here with a reason; empty means every target runs everywhere.
    struct NotRunInCi
    {
        std::string target;
        std::string lane;// "reusable-linux.yml", "Invoke-WindowsLane.ps1", or "Invoke-ClangClDebug.ps1"
        std::string reason;
    };
    const std::vector<NotRunInCi> kNotRunInCi;

    std::vector<std::string> missing;
    auto check_lane = [&](const std::string &lane_name, const std::set<std::string> &lane_set) {
        for (const auto &target : declared_targets) {
            if (lane_set.contains(target)) { continue; }
            const bool excused = std::any_of(kNotRunInCi.begin(), kNotRunInCi.end(), [&](const NotRunInCi &entry) {
                return entry.target == target && entry.lane == lane_name;
            });
            if (excused) { continue; }
            missing.push_back(target + " missing from " + lane_name);
        }
    };
    check_lane("reusable-linux.yml", linux_set);
    check_lane("Invoke-WindowsLane.ps1", windows_set);
    check_lane("Invoke-ClangClDebug.ps1", local_set);

    EXPECT_TRUE(missing.empty())
      << missing.size()
      << " fuzz target(s) declared in Test/fuzz/CMakeLists.txt do not run in every CI lane and are not listed "
         "in kNotRunInCi with a reason: "
      << joinViolations(missing);

    // A stale Linux entry fails the sanitizers job on its missing executable, so check both directions there too.
    std::vector<std::string> dead_linux_entries;
    for (const auto &target : *linux_targets_opt) {
        if (!declared_set.contains(target)) { dead_linux_entries.push_back(target); }
    }
    EXPECT_TRUE(dead_linux_entries.empty())
      << dead_linux_entries.size()
      << " entry/entries in reusable-linux.yml's fuzz-seed for-loop do not correspond to any target declared in "
         "Test/fuzz/CMakeLists.txt (renamed or deleted?): "
      << joinViolations(dead_linux_entries);

    // Nothing else catches a stale local-runner entry, so check both directions here.
    std::vector<std::string> dead_local_entries;
    for (const auto &target : *local_targets_opt) {
        if (!declared_set.contains(target)) { dead_local_entries.push_back(target); }
    }
    EXPECT_TRUE(dead_local_entries.empty())
      << dead_local_entries.size()
      << " entry/entries in Invoke-ClangClDebug.ps1's local fuzz-run foreach array do not correspond to any "
         "target declared in Test/fuzz/CMakeLists.txt (renamed or deleted?): "
      << joinViolations(dead_local_entries);
}

// Source-shape gate: each Invoke-ClangCl*.ps1 must `exit` with a variable, or a failed run reads as a clean quit.
TEST(BuildIntegrity, EveryHostRunnerPropagatesTheApplicationExitCode)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path scripts_dir = repo_root / "scripts" / "windows";
    ASSERT_TRUE(sourceExists(scripts_dir)) << "could not locate " << scripts_dir.string();

    std::vector<fs::path> runner_scripts;
    for (const auto &entry : fs::directory_iterator(scripts_dir)) {
        if (!entry.is_regular_file()) { continue; }
        const fs::path &candidate = entry.path();
        if (candidate.extension() == ".ps1" && candidate.filename().string().rfind("Invoke-ClangCl", 0) == 0) {
            runner_scripts.push_back(candidate);
        }
    }
    ASSERT_FALSE(runner_scripts.empty()) << "found zero Invoke-ClangCl*.ps1 helpers under " << scripts_dir.string()
                                         << " - the naming convention may have changed";

    static const std::regex kExitVariableLine(R"(^\s*exit\s+\$[A-Za-z_][A-Za-z0-9_:]*\s*$)");

    std::vector<std::string> missing_exit;
    for (const auto &script_path : runner_scripts) {
        const auto lines = readFileLines(script_path);
        if (!lines) {
            missing_exit.push_back(script_path.filename().string() + " (could not open)");
            continue;
        }

        bool found = false;
        for (const auto &line : *lines) {
            if (std::regex_match(line, kExitVariableLine)) {
                found = true;
                break;
            }
        }
        if (!found) { missing_exit.push_back(script_path.filename().string()); }
    }

    EXPECT_TRUE(missing_exit.empty())
      << missing_exit.size()
      << " Invoke-ClangCl*.ps1 helper(s) have no top-level 'exit $<variable>' line, so a failing launch inside "
         "them can silently report success to the caller: "
      << joinViolations(missing_exit);
}

// Compare-PerfBaseline.ps1 ignores one-sided entries, so a benchmark without a baseline row is silently never compared.
TEST(BuildIntegrity, PerfBaselineCoversEveryRegisteredBenchmark)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const std::vector<std::string> declared_benchmarks =
      parse_declared_perf_benchmarks(repo_root / "Test" / "perf" / "perfSuite.cpp");
    ASSERT_FALSE(declared_benchmarks.empty())
      << "parsed zero BENCHMARK(...) declarations out of Test/perf/perfSuite.cpp - the anchor text "
         "('BENCHMARK(') may have changed";
    const std::set<std::string> declared_set(declared_benchmarks.begin(), declared_benchmarks.end());

    const fs::path baseline_path = repo_root / "Test" / "perf" / "baselines" / "win-9070xt-32core.json";
    const auto baseline_names_opt = parse_perf_baseline_names(baseline_path);
    ASSERT_TRUE(baseline_names_opt.has_value())
      << "could not parse " << baseline_path.string() << " as Google Benchmark JSON";
    const std::vector<std::string> &baseline_names = *baseline_names_opt;
    ASSERT_FALSE(baseline_names.empty()) << "parsed zero benchmarks[] rows out of " << baseline_path.string()
                                         << " - is the file empty or malformed?";
    const std::set<std::string> baseline_set(baseline_names.begin(), baseline_names.end());

    static const char *const kRefreshHint =
      "see Compare-PerfBaseline.ps1's header for the refresh procedure (run the suite, eyeball the result, copy "
      "the JSON over by hand - there is deliberately no capture mode)";

    std::vector<std::string> missing_from_baseline;
    for (const auto &name : declared_benchmarks) {
        if (!baseline_set.contains(name)) { missing_from_baseline.push_back(name); }
    }
    EXPECT_TRUE(missing_from_baseline.empty())
      << missing_from_baseline.size() << " benchmark(s) registered in Test/perf/perfSuite.cpp have no row in "
      << baseline_path.string() << ", so Compare-PerfBaseline.ps1 silently never compares them (" << kRefreshHint
      << "): " << joinViolations(missing_from_baseline);

    std::vector<std::string> dead_baseline_rows;
    for (const auto &name : baseline_names) {
        if (!declared_set.contains(name)) { dead_baseline_rows.push_back(name); }
    }
    EXPECT_TRUE(dead_baseline_rows.empty())
      << dead_baseline_rows.size() << " row(s) in " << baseline_path.string()
      << " do not correspond to any BENCHMARK(...) currently registered in Test/perf/perfSuite.cpp (renamed or "
         "removed? "
      << kRefreshHint << "): " << joinViolations(dead_baseline_rows);
}

// Run names per BENCHMARK: the bare NAME, or "NAME/N" per ->Arg(N); other chained calls leave the name alone.
std::vector<std::string> parse_registered_benchmark_names(const std::string &perf_suite_text)
{
    static const std::regex kRegistration(R"(BENCHMARK\(\s*([A-Za-z_]\w*)\s*\)((?:\s*->\s*\w+\([^)]*\))*)\s*;)");
    static const std::regex kArg(R"(->\s*Arg\(\s*(\d+)\s*\))");

    std::vector<std::string> names;
    for (auto match = std::sregex_iterator(perf_suite_text.begin(), perf_suite_text.end(), kRegistration),
              match_end = std::sregex_iterator();
      match != match_end;
      ++match) {
        const std::string benchmark_name = (*match)[1].str();
        const std::string chain = (*match)[2].str();

        std::vector<std::string> args;
        for (auto arg_match = std::sregex_iterator(chain.begin(), chain.end(), kArg), arg_end = std::sregex_iterator();
          arg_match != arg_end;
          ++arg_match) {
            args.push_back((*arg_match)[1].str());
        }

        if (args.empty()) {
            names.push_back(benchmark_name);
        } else {
            for (const auto &arg : args) { names.push_back(benchmark_name + "/" + arg); }
        }
    }
    return names;
}

// The baseline must cover every registered run name, since Compare-PerfBaseline.ps1 never fails on a missing one.
TEST(BuildIntegrity, EveryRegisteredBenchmarkHasAPerfBaselineRow)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path perf_suite_path = repo_root / "Test" / "perf" / "perfSuite.cpp";
    const auto perf_suite_text = readFileText(perf_suite_path);
    ASSERT_TRUE(perf_suite_text.has_value()) << "missing " << perf_suite_path.string();

    const fs::path baseline_path = repo_root / "Test" / "perf" / "baselines" / "win-9070xt-32core.json";
    const auto baseline_text = readFileText(baseline_path);
    ASSERT_TRUE(baseline_text.has_value()) << "missing " << baseline_path.string();

    const nlohmann::json baseline_doc = nlohmann::json::parse(*baseline_text, nullptr, /*allow_exceptions=*/false);
    ASSERT_FALSE(baseline_doc.is_discarded()) << baseline_path.string() << " is not valid JSON";
    ASSERT_TRUE(baseline_doc.contains("benchmarks") && baseline_doc["benchmarks"].is_array())
      << baseline_path.string() << " has no \"benchmarks\" array";

    std::set<std::string> baseline_names;
    for (const auto &entry : baseline_doc["benchmarks"]) {
        ASSERT_TRUE(entry.contains("name") && entry["name"].is_string())
          << baseline_path.string() << " has a benchmarks[] entry with no string \"name\"";
        baseline_names.insert(entry["name"].get<std::string>());
    }

    const std::vector<std::string> registered = parse_registered_benchmark_names(*perf_suite_text);
    ASSERT_FALSE(registered.empty()) << "found no BENCHMARK(...) registrations in " << perf_suite_path.string();
    const std::set<std::string> registered_names(registered.begin(), registered.end());

    std::vector<std::string> missing;
    for (const auto &name : registered_names) {
        if (!baseline_names.contains(name)) { missing.push_back(name); }
    }
    std::vector<std::string> stale;
    for (const auto &name : baseline_names) {
        if (!registered_names.contains(name)) { stale.push_back(name); }
    }

    EXPECT_TRUE(missing.empty()) << perf_suite_path.string() << " registers benchmark(s) with no row in "
                                 << baseline_path.string() << ":" << joinViolations(missing);
    EXPECT_TRUE(stale.empty()) << baseline_path.string()
                               << " has benchmarks[] row(s) for benchmark(s) no longer registered in "
                               << perf_suite_path.string() << " - delete the stale row(s):" << joinViolations(stale);
}
