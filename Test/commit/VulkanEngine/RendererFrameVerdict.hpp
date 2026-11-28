// Compare-RendererPixels.ps1's verdict in C++, so compareRendererFrames and its suite share one copy of its lines.
#pragma once

#include "GoldenMetrics.hpp"

#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Kataglyphis::Test::RendererFrames {

using GoldenMetrics::FrameStructure;

// The PowerShell script's exit codes: 2 checked nothing, which is never a pass.
inline constexpr int kExitPassed = 0;
inline constexpr int kExitFailed = 1;
inline constexpr int kExitNothingChecked = 2;

struct Verdict
{
    int exit_code = kExitPassed;
    std::vector<std::string> lines;
};

// One renderer's three checks; false when any failed.
inline bool append_structure_lines(std::vector<std::string> &lines, std::string_view label, const FrameStructure &frame)
{
    const double lit_percent = frame.lit_fraction * 100.0;
    lines.push_back(frame.has_structure()
                      ? std::format("OK   [{}] Frame has structure (stddev {:.2f}).", label, frame.stddev)
                      : std::format("FAIL [{}] Frame is essentially uniform (stddev {:.2f}).", label, frame.stddev));
    lines.push_back(frame.is_lit()
                      ? std::format("OK   [{}] Frame is well-lit (lit {:.1f}%).", label, lit_percent)
                      : std::format("FAIL [{}] Frame is too dark (lit {:.1f}% of pixels).", label, lit_percent));
    lines.push_back(frame.has_variety()
                      ? std::format("OK   [{}] Good luminance variety ({} buckets).", label, frame.buckets)
                      : std::format("FAIL [{}] Too few luminance buckets ({}).", label, frame.buckets));
    return frame.passes();
}

inline std::string metrics_row(std::string_view renderer, const FrameStructure &frame)
{
    return std::format("{:<12} {:>9.2f} {:>7.2f} {:>8} {:>6.1f}%",
      renderer,
      frame.mean,
      frame.stddev,
      frame.buckets,
      frame.lit_fraction * 100.0);
}

// A missing frame is graded as absent, as the PowerShell script does; only no frame at all checks nothing.
inline Verdict grade_renderer_frames(const std::optional<FrameStructure> &cpp,
  const std::optional<FrameStructure> &rust)
{
    Verdict verdict;
    auto &lines = verdict.lines;
    lines.emplace_back("=== Metrics ===");
    if (cpp || rust) {
        lines.push_back(
          std::format("{:<12} {:>9} {:>7} {:>8} {:>7}", "Renderer", "Mean Lum", "StdDev", "Buckets", "Lit%"));
    }
    if (cpp) {
        lines.push_back(metrics_row("C++/Vulkan", *cpp));
    } else {
        lines.emplace_back("C++ frame not available.");
    }
    if (rust) {
        lines.push_back(metrics_row("Rust/WebGPU", *rust));
    } else {
        lines.emplace_back("Rust frame not available.");
    }

    if (!cpp && !rust) {
        lines.emplace_back("No frames were captured or found - nothing was checked.");
        lines.emplace_back("=== PIXEL COMPARISON: NOTHING CHECKED (exit code 2) ===");
        verdict.exit_code = kExitNothingChecked;
        return verdict;
    }

    lines.emplace_back("=== Assertions ===");
    bool passed = true;
    if (cpp && !append_structure_lines(lines, "C++", *cpp)) { passed = false; }
    if (rust && !append_structure_lines(lines, "Rust", *rust)) { passed = false; }

    if (cpp && rust) {
        lines.emplace_back("=== Cross-Renderer Comparison ===");
        lines.push_back(std::format("Mean luminance diff: {:.2f} (C++: {:.2f}, Rust: {:.2f})",
          std::abs(cpp->mean - rust->mean),
          cpp->mean,
          rust->mean));
        lines.push_back(std::format("StdDev diff:         {:.2f}", std::abs(cpp->stddev - rust->stddev)));
        const double ratio = GoldenMetrics::renderer_mean_ratio(*cpp, *rust);
        lines.push_back(std::format("Mean luminance ratio (Rust/C++): {:.2f}", ratio));
        if (GoldenMetrics::renderer_mean_ratio_holds(ratio)) {
            lines.emplace_back("OK   Luminance ratio within tolerance.");
        } else {
            lines.push_back(std::format("FAIL Luminance ratio {:.2f} out of expected range [{:.1f}, {:.1f}].",
              ratio,
              GoldenMetrics::kRendererMeanRatioMin,
              GoldenMetrics::kRendererMeanRatioMax));
            lines.emplace_back("      This may indicate one renderer producing a blank or degenerate frame.");
            passed = false;
        }
    }

    verdict.exit_code = passed ? kExitPassed : kExitFailed;
    lines.emplace_back(passed ? "=== PIXEL COMPARISON PASSED ===" : "=== PIXEL COMPARISON FAILED (exit code 1) ===");
    return verdict;
}

}// namespace Kataglyphis::Test::RendererFrames
