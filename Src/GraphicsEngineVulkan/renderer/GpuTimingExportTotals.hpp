#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// A plain header, not a module, so scripts/linux/tests/test-gpu-timing-export.sh can compile it with any C++ compiler.
namespace Kataglyphis::VulkanRendererInternals {

// The KATAGLYPHIS_GPU_TIMING_JSON totals: raw per-pass samples since the last restart, never the load frame's.
template<std::size_t PassCount> class GpuTimingExportTotals
{
  public:
    using PassSums = std::array<double, PassCount>;
    using PassSamples = std::array<std::uint64_t, PassCount>;
    // One slice read back, in milliseconds per pass; a negative entry is a pass that slice did not measure.
    using SliceMs = std::array<double, PassCount>;

    // A new swapchain's slices hold no frame yet; a load frame still to come marks whichever records first.
    void resizeSlices(std::size_t imageCount) { slice_holds_load.assign(imageCount, false); }

    // New geometry: the next frame recorded draws it first, and on llvmpipe compiles its pipelines inside the frame.
    void restart()
    {
        sum_ms.fill(0.0);
        samples.fill(0U);
        frames = 0;
        slice_holds_load.assign(slice_holds_load.size(), false);
        load_frame_pending = true;
    }

    // Marked by recording, not by readback: a mailbox chain may read another image's slice back first.
    void sliceRecorded(std::size_t imageIndex)
    {
        if (imageIndex >= slice_holds_load.size()) { return; }
        slice_holds_load[imageIndex] = std::exchange(load_frame_pending, false);
    }

    // A recording that never reached the GPU compiled nothing, so the next one is still the load frame.
    void sliceDiscarded(std::size_t imageIndex)
    {
        if (imageIndex >= slice_holds_load.size()) { return; }
        if (slice_holds_load[imageIndex]) { load_frame_pending = true; }
        slice_holds_load[imageIndex] = false;
    }

    // The mark stays until the slice is recorded again, so reading the load frame twice still adds nothing.
    void addSlice(std::size_t imageIndex, const SliceMs &slice_ms)
    {
        if (sliceHoldsLoad(imageIndex)) { return; }
        bool measured = false;
        for (std::size_t pass = 0; pass < PassCount; ++pass) {
            if (slice_ms[pass] < 0.0) { continue; }
            sum_ms[pass] += slice_ms[pass];
            samples[pass]++;
            measured = true;
        }
        if (measured) { frames++; }
    }

    [[nodiscard]] bool sliceHoldsLoad(std::size_t imageIndex) const
    { return imageIndex < slice_holds_load.size() && slice_holds_load[imageIndex]; }
    [[nodiscard]] const PassSums &sums() const { return sum_ms; }
    [[nodiscard]] const PassSamples &sampleCounts() const { return samples; }
    [[nodiscard]] std::uint64_t framesMeasured() const { return frames; }

  private:
    PassSums sum_ms{};
    PassSamples samples{};
    std::uint64_t frames{ 0 };
    // Per swapchain image: whether its slice holds the first frame recorded after restart().
    std::vector<bool> slice_holds_load;
    bool load_frame_pending{ false };
};

}// namespace Kataglyphis::VulkanRendererInternals
