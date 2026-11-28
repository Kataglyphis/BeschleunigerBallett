// The path tracer's dispatch plan: Rust's dispatch.rs tests, and the budget the editor's maxima must keep.

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "renderer/PathTracingDispatch.hpp"

using Kataglyphis::kPathTracingDispatchSegments;
using Kataglyphis::kPathTracingTileSize;
using Kataglyphis::PathTracingDispatch;
using Kataglyphis::pathTracingDispatchBudget;
using Kataglyphis::pathTracingSamplesPerDispatch;
using Kataglyphis::planPathTracingDispatches;

namespace {

// Samples each pixel gets over `dispatches`, row-major.
std::vector<uint32_t> coverage(uint32_t width, uint32_t height, const std::vector<PathTracingDispatch> &dispatches)
{
    std::vector<uint32_t> samples(static_cast<std::size_t>(width) * height, 0U);
    for (const PathTracingDispatch &d : dispatches) {
        for (uint32_t y = d.y; y < d.y + d.height; ++y) {
            for (uint32_t x = d.x; x < d.x + d.width; ++x) {
                samples[static_cast<std::size_t>(y) * width + x] += d.samples;
            }
        }
    }
    return samples;
}

}// namespace

TEST(PathTracingDispatchUnit, EveryPixelGetsEverySampleExactlyOnce)
{
    struct Case
    {
        uint32_t width;
        uint32_t height;
        uint32_t samples;
        uint32_t bounces;
    };
    for (const Case c :
      { Case{ 640, 360, 8, 8 }, Case{ 300, 1, 64, 16 }, Case{ 1, 1, 1, 1 }, Case{ 1200, 768, 64, 16 } }) {
        const std::vector<uint32_t> got =
          coverage(c.width, c.height, planPathTracingDispatches(c.width, c.height, c.samples, c.bounces, 0));
        std::size_t wrong = 0;
        for (const uint32_t n : got) { wrong += n == c.samples ? 0U : 1U; }
        EXPECT_EQ(wrong, 0U) << c.width << " x " << c.height << " at " << c.samples << " samples";
    }
}

TEST(PathTracingDispatchUnit, NoDispatchTracesPastTheBudget)
{
    for (const auto &[samples, bounces] :
      { std::pair{ 64U, 16U }, std::pair{ 8U, 8U }, std::pair{ 1U, 16U }, std::pair{ 64U, 1U } }) {
        for (const PathTracingDispatch &d : planPathTracingDispatches(1920, 1080, samples, bounces, 0)) {
            const uint64_t segments = uint64_t{ d.width } * d.height * d.samples * bounces;
            EXPECT_LE(segments, kPathTracingDispatchSegments) << d.x << ", " << d.y << " at " << bounces << " bounces";
        }
    }
    EXPECT_EQ(pathTracingSamplesPerDispatch(8, 8), 8U);
    EXPECT_EQ(pathTracingSamplesPerDispatch(64, 16), 4U);
}

TEST(PathTracingDispatchUnit, AChunkAddsToTheSamplesTheEarlierOnesLeft)
{
    const std::vector<PathTracingDispatch> dispatches = planPathTracingDispatches(300, 200, 64, 16, 1000);
    std::vector<std::pair<uint32_t, uint32_t>> firsts;
    for (const PathTracingDispatch &d : dispatches) {
        if (d.x == 0 && d.y == 0) { firsts.emplace_back(d.first_sample, d.accumulated); }
    }
    std::vector<std::pair<uint32_t, uint32_t>> expected;
    for (uint32_t chunk = 0; chunk < 16U; ++chunk) { expected.emplace_back(chunk * 4U, 1000U + chunk * 4U); }
    EXPECT_EQ(firsts, expected);
    EXPECT_EQ(dispatches.size(), 16U * 2U);
}

// The editor's maxima, 64 paths of 16 bounces, put about 2,000 rays per pixel into one unplanned dispatch.
TEST(PathTracingDispatchUnit, TheEditorsMaximaSplitIntoChunksOfWholeTiles)
{
    const std::vector<PathTracingDispatch> dispatches = planPathTracingDispatches(1200, 768, 64, 16, 0);
    // 5 x 3 tiles of 256, the last column 176 wide, and 16 chunks of 4 samples.
    ASSERT_EQ(dispatches.size(), 16U * 15U);
    EXPECT_EQ(dispatches[4].x, 4U * kPathTracingTileSize);
    EXPECT_EQ(dispatches[4].width, 176U);
    EXPECT_EQ(dispatches[14].height, kPathTracingTileSize);
    for (const PathTracingDispatch &d : dispatches) { EXPECT_EQ(d.samples, 4U); }
    EXPECT_EQ(dispatches.back().first_sample, 60U);
}

// A smaller budget only splits the samples further; the coverage and each chunk's starting count stay exact.
TEST(PathTracingDispatchUnit, ASmallerBudgetSplitsTheSamplesFurther)
{
    const uint64_t one_sample = uint64_t{ kPathTracingTileSize } * kPathTracingTileSize * 2U;
    const std::vector<PathTracingDispatch> dispatches = planPathTracingDispatches(600, 300, 4, 2, 12, one_sample);
    ASSERT_EQ(dispatches.size(), 4U * 6U);
    for (std::size_t i = 0; i < dispatches.size(); ++i) {
        EXPECT_EQ(dispatches[i].samples, 1U);
        EXPECT_EQ(dispatches[i].first_sample, i / 6U);
        EXPECT_EQ(dispatches[i].accumulated, 12U + i / 6U);
    }
    std::size_t wrong = 0;
    for (const uint32_t n : coverage(600, 300, dispatches)) { wrong += n == 4U ? 0U : 1U; }
    EXPECT_EQ(wrong, 0U);
    // A budget below one sample of a tile still traces a sample per dispatch.
    EXPECT_EQ(pathTracingSamplesPerDispatch(4, 2, 1), 1U);
}

// KATAGLYPHIS_PT_DISPATCH_SEGMENTS only lowers the budget: a larger or wrapped value would lift the chunk split.
TEST(PathTracingDispatchUnit, TheBudgetVariableOnlyLowersTheBudget)
{
    EXPECT_EQ(pathTracingDispatchBudget("131072"), 131072U);
    EXPECT_EQ(pathTracingDispatchBudget("1"), 1U);
    EXPECT_EQ(pathTracingDispatchBudget("4194303"), kPathTracingDispatchSegments - 1U);
    for (const char *const kept : { "",
           "0",
           "4194304",
           "4194305",
           "99999999999",
           "18446744073709551615",
           "18446744073709551616",
           "-1",
           "-5",
           "+12",
           " 12",
           "12abc",
           "abc" }) {
        EXPECT_EQ(pathTracingDispatchBudget(kept), kPathTracingDispatchSegments) << "'" << kept << "'";
    }
    // Kept, the editor's maxima still split into Rust's chunks of four samples.
    const uint64_t budget = pathTracingDispatchBudget("99999999999");
    EXPECT_EQ(planPathTracingDispatches(1200, 768, 64, 16, 0, budget).size(), 16U * 15U);
}

// The count the shader weighs the history by saturates instead of wrapping to an overwrite.
TEST(PathTracingDispatchUnit, TheAccumulatedCountSaturates)
{
    constexpr uint64_t kPast = uint64_t{ std::numeric_limits<uint32_t>::max() } + 5U;
    for (const PathTracingDispatch &d : planPathTracingDispatches(16, 16, 8, 1, kPast)) {
        EXPECT_EQ(d.accumulated, std::numeric_limits<uint32_t>::max());
    }
    // Degenerate input still plans one sample over one pixel.
    const std::vector<PathTracingDispatch> empty = planPathTracingDispatches(0, 0, 0, 0, 0);
    ASSERT_EQ(empty.size(), 1U);
    EXPECT_EQ(empty[0].width * empty[0].height * empty[0].samples, 1U);
}
