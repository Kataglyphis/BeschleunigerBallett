// compareRendererFrames speaks through this verdict, so its exit codes and lines are tested without a GPU or a PNG.

#include <gtest/gtest.h>

#include "RendererFrameVerdict.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Kataglyphis::Test::GoldenMetrics::FrameStructure;
using Kataglyphis::Test::RendererFrames::grade_renderer_frames;
using Kataglyphis::Test::RendererFrames::kExitFailed;
using Kataglyphis::Test::RendererFrames::kExitNothingChecked;
using Kataglyphis::Test::RendererFrames::kExitPassed;
using Kataglyphis::Test::RendererFrames::Verdict;

constexpr FrameStructure kStructured{ 100.0, 40.0, 0.9, 200U };
constexpr FrameStructure kFlatBlack{ 0.0, 0.0, 0.0, 1U };

bool has_line_starting(const Verdict &verdict, std::string_view prefix)
{
    return std::ranges::any_of(verdict.lines, [prefix](const std::string &line) { return line.starts_with(prefix); });
}

}// namespace

TEST(RendererFrameVerdict, PassesTwoStructuredFramesWithinTheRatio)
{
    FrameStructure brighter = kStructured;
    brighter.mean = 148.0;
    const Verdict verdict = grade_renderer_frames(kStructured, brighter);
    EXPECT_EQ(verdict.exit_code, kExitPassed);
    for (const std::string_view label : { "[C++]", "[Rust]" }) {
        EXPECT_TRUE(has_line_starting(verdict, std::string("OK   ") + std::string(label) + " Frame has structure"));
        EXPECT_TRUE(has_line_starting(verdict, std::string("OK   ") + std::string(label) + " Frame is well-lit"));
        EXPECT_TRUE(has_line_starting(verdict, std::string("OK   ") + std::string(label) + " Good luminance variety"));
    }
    EXPECT_TRUE(has_line_starting(verdict, "Mean luminance ratio (Rust/C++): 1.48"));
    EXPECT_TRUE(has_line_starting(verdict, "OK   Luminance ratio within tolerance."));
    EXPECT_EQ(verdict.lines.back(), "=== PIXEL COMPARISON PASSED ===");
}

TEST(RendererFrameVerdict, FailsEachRendererOnlyOnItsOwnChecks)
{
    const Verdict verdict = grade_renderer_frames(kStructured, kFlatBlack);
    EXPECT_EQ(verdict.exit_code, kExitFailed);
    EXPECT_FALSE(has_line_starting(verdict, "FAIL [C++]"));
    EXPECT_TRUE(has_line_starting(verdict, "FAIL [Rust] Frame is essentially uniform"));
    EXPECT_TRUE(has_line_starting(verdict, "FAIL [Rust] Frame is too dark"));
    EXPECT_TRUE(has_line_starting(verdict, "FAIL [Rust] Too few luminance buckets"));
    EXPECT_EQ(verdict.lines.back(), "=== PIXEL COMPARISON FAILED (exit code 1) ===");
}

// An empty run is never a pass: exit 2, as Compare-RendererPixels.ps1 has it.
TEST(RendererFrameVerdict, ChecksNothingWithoutAFrame)
{
    const Verdict verdict = grade_renderer_frames(std::nullopt, std::nullopt);
    EXPECT_EQ(verdict.exit_code, kExitNothingChecked);
    EXPECT_FALSE(has_line_starting(verdict, "=== PIXEL COMPARISON PASSED"));
    EXPECT_FALSE(has_line_starting(verdict, "=== Assertions"));
    EXPECT_EQ(verdict.lines.back(), "=== PIXEL COMPARISON: NOTHING CHECKED (exit code 2) ===");
}

TEST(RendererFrameVerdict, GradesALoneFrameWithoutTheRatio)
{
    const Verdict verdict = grade_renderer_frames(kStructured, std::nullopt);
    EXPECT_EQ(verdict.exit_code, kExitPassed);
    EXPECT_TRUE(has_line_starting(verdict, "Rust frame not available."));
    EXPECT_FALSE(has_line_starting(verdict, "=== Cross-Renderer Comparison"));

    const Verdict rust_alone = grade_renderer_frames(std::nullopt, kFlatBlack);
    EXPECT_EQ(rust_alone.exit_code, kExitFailed);
    EXPECT_TRUE(has_line_starting(rust_alone, "C++ frame not available."));
}

TEST(RendererFrameVerdict, FailsAMeanRatioOutsideItsRange)
{
    FrameStructure dim = kStructured;
    dim.mean = 20.0;
    const Verdict verdict = grade_renderer_frames(dim, kStructured);
    EXPECT_EQ(verdict.exit_code, kExitFailed);
    EXPECT_TRUE(has_line_starting(verdict, "FAIL Luminance ratio 5.00 out of expected range [0.5, 4.0]."));
    EXPECT_FALSE(has_line_starting(verdict, "FAIL [C++]"));
    EXPECT_FALSE(has_line_starting(verdict, "FAIL [Rust]"));
}
