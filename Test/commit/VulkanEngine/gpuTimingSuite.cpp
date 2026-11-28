#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include "renderer/GpuTimingExportTotals.hpp"

import kataglyphis.vulkan.gpu_timing;
import kataglyphis.vulkan.gui_renderer_shared_vars;

namespace {
using Kataglyphis::GpuTimingSubsystem;
namespace FrontendShared = Kataglyphis::VulkanRendererInternals::FrontendShared;
using OnePassTotals = Kataglyphis::VulkanRendererInternals::GpuTimingExportTotals<1>;

// Each frame reads its image's last slice back, as readTimings does, then records its own, as endFrame does.
class FakeTimingSequence
{
  public:
    explicit FakeTimingSequence(size_t images) : slice_ms_(images, -1.0) { totals_.resizeSlices(images); }

    void restart()
    {
        totals_.restart();
        slice_ms_.assign(slice_ms_.size(), -1.0);
    }

    void frame(size_t image, double ms)
    {
        if (slice_ms_[image] >= 0.0) { totals_.addSlice(image, { slice_ms_[image] }); }
        slice_ms_[image] = ms;
        totals_.sliceRecorded(image);
    }

    // recordCommandsUnsubmittedForBenchmark: recorded, never submitted, so nothing reads it back.
    void recordUnsubmitted(size_t image)
    {
        totals_.sliceRecorded(image);
        totals_.sliceDiscarded(image);
        slice_ms_[image] = -1.0;
    }

    [[nodiscard]] double meanMs() const
    {
        const uint64_t samples = totals_.sampleCounts()[0];
        return samples == 0U ? -1.0 : totals_.sums()[0] / static_cast<double>(samples);
    }

    [[nodiscard]] uint64_t framesMeasured() const { return totals_.framesMeasured(); }

  private:
    OnePassTotals totals_;
    std::vector<double> slice_ms_;
};
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

// New geometry restarts what the editor shows; a reading kept from the old scene would sit beside the new one's.
TEST(GpuTimingUnit, RestartClearsEveryPublishedReading)
{
    FrontendShared::GpuTimings timings;
    timings.pass_ms.fill(12.5F);
    GpuTimingSubsystem subsystem;
    subsystem.restart(timings);
    for (const float pass_ms : timings.pass_ms) { EXPECT_EQ(pass_ms, -1.0F); }
}

// A restart that marked no slice, or the wrong one, would leave the load frame's compile in every budget dump.
TEST(GpuTimingUnit, RestartMarksTheSliceTheNextFrameRecords)
{
    FrontendShared::GpuTimings timings;
    GpuTimingSubsystem subsystem;
    subsystem.sizeSlices(3U);
    subsystem.setPassRecordedMask(0U, 1U);
    EXPECT_FALSE(subsystem.exportTotals().sliceHoldsLoad(0U)) << "no geometry arrived, so no frame is its load frame";

    subsystem.restart(timings);
    subsystem.setPassRecordedMask(2U, 1U);
    subsystem.setPassRecordedMask(0U, 1U);
    EXPECT_TRUE(subsystem.exportTotals().sliceHoldsLoad(2U)) << "the first slice recorded after restart() is the load";
    EXPECT_FALSE(subsystem.exportTotals().sliceHoldsLoad(0U));

    // The benchmark's recording never reaches the GPU, so the load frame is still to come.
    subsystem.restart(timings);
    subsystem.setPassRecordedMask(2U, 1U);
    subsystem.clearPassRecordedMask(2U);
    subsystem.setPassRecordedMask(1U, 1U);
    EXPECT_FALSE(subsystem.exportTotals().sliceHoldsLoad(2U));
    EXPECT_TRUE(subsystem.exportTotals().sliceHoldsLoad(1U));
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

// On llvmpipe the load frame compiles the pass inside it (Forward 1070 ms, then 525-545): a dump kept it for good.
TEST(GpuTimingExportUnit, TheDumpLeavesOutTheSliceTheLoadFrameRecorded)
{
    FakeTimingSequence run(3U);
    for (const size_t image : { 0U, 1U, 2U, 0U }) { run.frame(image, 9.0); }
    run.restart();

    // A mailbox chain may hand image 1 back first; image 0 holds the load frame and comes back third.
    constexpr std::array<size_t, 8> ORDER{ 0U, 1U, 2U, 1U, 2U, 0U, 1U, 2U };
    for (size_t frame = 0; frame < ORDER.size(); ++frame) { run.frame(ORDER[frame], frame == 0U ? 1000.0 : 500.0); }

    // Five slices came back: the sky-only frames went with the restart and the load frame stays out.
    EXPECT_EQ(run.framesMeasured(), 4U);
    EXPECT_DOUBLE_EQ(run.meanMs(), 500.0);
}

TEST(GpuTimingExportUnit, ARecordingNeverSubmittedLeavesTheLoadFrameToTheNextOne)
{
    FakeTimingSequence run(2U);
    run.restart();
    run.recordUnsubmitted(0U);
    run.frame(1U, 1000.0);
    for (const size_t image : { 0U, 1U, 0U }) { run.frame(image, 500.0); }

    EXPECT_EQ(run.framesMeasured(), 1U) << "the load frame came after the unsubmitted recording";
    EXPECT_DOUBLE_EQ(run.meanMs(), 500.0);
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
