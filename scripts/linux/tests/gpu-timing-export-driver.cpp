// test-gpu-timing-export.sh's driver: one fake timing sequence per argument against GpuTimingExportTotals.hpp.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "renderer/GpuTimingExportTotals.hpp"

using Totals = Kataglyphis::VulkanRendererInternals::GpuTimingExportTotals<1>;

namespace {

// Each frame reads its image's last slice back, as readTimings does, then records its own, as endFrame does.
class Sequence
{
  public:
    explicit Sequence(std::size_t images) : slice_ms_(images, -1.0) { totals_.resizeSlices(images); }

    void restart()
    {
        totals_.restart();
        slice_ms_.assign(slice_ms_.size(), -1.0);
    }

    void frame(std::size_t image, double ms)
    {
        if (slice_ms_[image] >= 0.0) { totals_.addSlice(image, { slice_ms_[image] }); }
        slice_ms_[image] = ms;
        totals_.sliceRecorded(image);
    }

    void recordUnsubmitted(std::size_t image)
    {
        totals_.sliceRecorded(image);
        totals_.sliceDiscarded(image);
        slice_ms_[image] = -1.0;
    }

    [[nodiscard]] auto holds(uint64_t frames, double mean_ms) const -> bool
    {
        const uint64_t samples = totals_.sampleCounts()[0];
        const double mean = samples == 0U ? -1.0 : totals_.sums()[0] / static_cast<double>(samples);
        std::fprintf(
          stderr, "frames %llu, mean %.1f ms\n", static_cast<unsigned long long>(totals_.framesMeasured()), mean);
        return totals_.framesMeasured() == frames && mean == mean_ms;
    }

  private:
    Totals totals_;
    std::vector<double> slice_ms_;
};

auto expect(bool holds, const char *what) -> int
{
    if (!holds) { std::fprintf(stderr, "FAIL: %s\n", what); }
    return holds ? 0 : 1;
}

// Image 0 draws the load frame (1000 ms), but a mailbox chain hands image 1 back first.
auto loadFrame() -> int
{
    Sequence run(3U);
    run.restart();
    const std::size_t order[] = { 0U, 1U, 2U, 1U, 2U, 0U, 1U, 2U };
    for (std::size_t frame = 0; frame < 8U; ++frame) { run.frame(order[frame], frame == 0U ? 1000.0 : 500.0); }
    return expect(run.holds(4U, 500.0), "four of five slices count, the load frame's stays out");
}

auto unsubmitted() -> int
{
    Sequence run(2U);
    run.restart();
    run.recordUnsubmitted(0U);
    run.frame(1U, 1000.0);
    for (const std::size_t image : { 0U, 1U, 0U }) { run.frame(image, 500.0); }
    return expect(run.holds(1U, 500.0), "the frame after an unsubmitted recording is the load frame");
}

auto restarted() -> int
{
    Sequence run(2U);
    for (const std::size_t image : { 0U, 1U, 0U, 1U }) { run.frame(image, 9.0); }
    run.restart();
    for (const std::size_t image : { 0U, 1U, 0U, 1U, 0U }) { run.frame(image, image == 0U ? 500.0 : 700.0); }
    return expect(run.holds(2U, 600.0), "the sky-only frames went with the restart");
}

}// namespace

auto main(int argc, char **argv) -> int
{
    const std::string scenario = argc > 1 ? argv[1] : "";
    if (scenario == "load-frame") { return loadFrame(); }
    if (scenario == "unsubmitted") { return unsubmitted(); }
    if (scenario == "restart") { return restarted(); }
    std::fprintf(stderr, "unknown scenario '%s'\n", scenario.c_str());
    return 2;
}
