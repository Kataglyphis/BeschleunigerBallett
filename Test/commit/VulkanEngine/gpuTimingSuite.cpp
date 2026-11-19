#include <array>
#include <cstdint>
#include <cstring>

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

import kataglyphis.vulkan.gpu_timing;
import kataglyphis.vulkan.gui_renderer_shared_vars;

namespace {
using Kataglyphis::GpuTimingSubsystem;
namespace FrontendShared = Kataglyphis::VulkanRendererInternals::FrontendShared;
}// namespace

TEST(GpuTimingUnit, AverageConvergesToMean)
{
    GpuTimingSubsystem::GpuPassAverage average;
    float last = 0.0F;
    for (uint32_t i = 0; i < GpuTimingSubsystem::GpuPassAverage::WINDOW; i++) { last = average.add(10.0F); }
    EXPECT_FLOAT_EQ(last, 10.0F);
}

TEST(GpuTimingUnit, WindowRollsOldestSample)
{
    GpuTimingSubsystem::GpuPassAverage average;
    for (uint32_t i = 0; i < GpuTimingSubsystem::GpuPassAverage::WINDOW; i++) { average.add(10.0F); }

    // One sample past the window evicts the oldest (10.0).
    const float mean = average.add(20.0F);
    constexpr auto WINDOW = static_cast<float>(GpuTimingSubsystem::GpuPassAverage::WINDOW);
    constexpr float EXPECTED = (((WINDOW - 1.0F) * 10.0F) + 20.0F) / WINDOW;
    EXPECT_NEAR(mean, EXPECTED, 1e-4F);
}

TEST(GpuTimingUnit, ResetClearsHistory)
{
    GpuTimingSubsystem::GpuPassAverage average;
    for (uint32_t i = 0; i < GpuTimingSubsystem::GpuPassAverage::WINDOW; i++) { average.add(10.0F); }

    average.reset();
    EXPECT_EQ(average.count, 0U);

    // A struct that never evicts would still blend in the pre-reset history.
    const float mean = average.add(20.0F);
    EXPECT_FLOAT_EQ(mean, 20.0F);
}

// A zero-filled or mismatched pass table compiles fine but yields a null name or a silent 0.000 ms.
TEST(GpuTimingTablesUnit, EveryPassHasBothADisplayAndAnExportName)
{
    for (int i = 0; i < FrontendShared::GPU_TIMED_PASS_COUNT; i++) {
        ASSERT_NE(FrontendShared::GPU_TIMED_PASS_NAMES[static_cast<size_t>(i)], nullptr);
        EXPECT_STRNE(FrontendShared::GPU_TIMED_PASS_NAMES[static_cast<size_t>(i)], "");
        ASSERT_NE(FrontendShared::GPU_TIMED_PASS_EXPORT_NAMES[static_cast<size_t>(i)], nullptr);
        EXPECT_STRNE(FrontendShared::GPU_TIMED_PASS_EXPORT_NAMES[static_cast<size_t>(i)], "");
    }
}

TEST(GpuTimingTablesUnit, ExportNamesAreUniqueAndContainNoSpaces)
{
    for (int i = 0; i < FrontendShared::GPU_TIMED_PASS_COUNT; i++) {
        const char *name = FrontendShared::GPU_TIMED_PASS_EXPORT_NAMES[static_cast<size_t>(i)];
        EXPECT_EQ(std::strchr(name, ' '), nullptr) << "export name '" << name << "' contains a space";
        for (int j = i + 1; j < FrontendShared::GPU_TIMED_PASS_COUNT; j++) {
            EXPECT_STRNE(name, FrontendShared::GPU_TIMED_PASS_EXPORT_NAMES[static_cast<size_t>(j)])
              << "export names at " << i << " and " << j << " collide";
        }
    }
}

TEST(GpuTimingTablesUnit, DefaultTimingsReportNoSampleForEveryPass)
{
    const FrontendShared::GpuTimings timings;
    for (float sample : timings.pass_ms) { EXPECT_LT(sample, 0.0F); }
}

TEST(GpuTimingTablesUnit, PassCountMatchesTheEnum)
{ EXPECT_EQ(static_cast<int>(FrontendShared::GpuTimedPass::Count), FrontendShared::GPU_TIMED_PASS_COUNT); }

// Main encloses the scope that drew; Forward, GBuffer, DeferredLighting, RayTrace and PathTrace are the Rust names.
TEST(GpuTimingTablesUnit, TheDrawingScopesNestInsideMainAndNothingNestsDeeper)
{
    using FrontendShared::GpuTimedPass;
    for (const GpuTimedPass nested : { GpuTimedPass::Forward,
           GpuTimedPass::GBuffer,
           GpuTimedPass::DeferredLighting,
           GpuTimedPass::RayTrace,
           GpuTimedPass::PathTrace }) {
        EXPECT_EQ(FrontendShared::GPU_TIMED_PASS_PARENTS[static_cast<size_t>(nested)], GpuTimedPass::Main)
          << FrontendShared::GPU_TIMED_PASS_EXPORT_NAMES[static_cast<size_t>(nested)];
    }
    for (const GpuTimedPass parent : FrontendShared::GPU_TIMED_PASS_PARENTS) {
        if (parent == GpuTimedPass::Count) { continue; }
        EXPECT_EQ(FrontendShared::GPU_TIMED_PASS_PARENTS[static_cast<size_t>(parent)], GpuTimedPass::Count)
          << FrontendShared::GPU_TIMED_PASS_EXPORT_NAMES[static_cast<size_t>(parent)] << " encloses a pass but nests";
    }
}

TEST(GpuTimingExportUnit, TheExportNamesEachMeasuredNestedPassesParent)
{
    using FrontendShared::GpuTimedPass;
    GpuTimingSubsystem::PassSums sums{};
    GpuTimingSubsystem::PassSamples samples{};
    const auto measure = [&](GpuTimedPass pass, double total_ms) {
        sums[static_cast<size_t>(pass)] = total_ms;
        samples[static_cast<size_t>(pass)] = 2U;
    };
    measure(GpuTimedPass::Main, 10.0);
    measure(GpuTimedPass::Forward, 8.0);
    measure(GpuTimedPass::Post, 4.0);

    const nlohmann::json dump = GpuTimingSubsystem::timingExport(sums, samples, 2U, true);
    ASSERT_TRUE(dump.contains("passes") && dump.contains("nested_in")) << dump.dump(2);

    // value(), not operator[]: a const json aborts on a missing key, where a test must fail.
    EXPECT_DOUBLE_EQ(dump["passes"].value("Main", -1.0), 5.0);
    EXPECT_DOUBLE_EQ(dump["passes"].value("Forward", -1.0), 4.0);
    EXPECT_EQ(dump["nested_in"].value("Forward", ""), "Main");
    EXPECT_FALSE(dump["nested_in"].contains("Main")) << "a top-level pass names no parent";
    EXPECT_FALSE(dump["nested_in"].contains("Post")) << "a top-level pass names no parent";
    EXPECT_FALSE(dump["nested_in"].contains("GBuffer")) << "an unmeasured pass stays out of the export";
    EXPECT_FALSE(dump["passes"].contains("GBuffer")) << "an unmeasured pass stays out of the export";
}

TEST(GpuTimingExportUnit, AnUnmeasuredRunStillWritesEmptyTables)
{
    const nlohmann::json dump =
      GpuTimingSubsystem::timingExport(GpuTimingSubsystem::PassSums{}, GpuTimingSubsystem::PassSamples{}, 0U, false);
    ASSERT_TRUE(dump.contains("passes") && dump.contains("nested_in")) << dump.dump(2);
    EXPECT_EQ(dump["frames_measured"], 0U);
    EXPECT_EQ(dump["timestamps_supported"], false);
    EXPECT_TRUE(dump["passes"].is_object() && dump["passes"].empty());
    EXPECT_TRUE(dump["nested_in"].is_object() && dump["nested_in"].empty());
}
