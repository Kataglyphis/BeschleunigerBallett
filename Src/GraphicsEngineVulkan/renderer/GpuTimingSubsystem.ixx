module;

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "spdlog/spdlog.h"

export module kataglyphis.vulkan.gpu_timing;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.gui_renderer_shared_vars;

namespace FrontendShared = Kataglyphis::VulkanRendererInternals::FrontendShared;

export namespace Kataglyphis {

// Owns the renderer's per-pass GPU timing (timestamp query pool), extracted
// verbatim from VulkanRenderer, mirroring the FrameSync extraction.
//
// Layout: one slice of QUERIES_PER_IMAGE queries per swapchain image, two
// queries (start/end) per timed pass. A frame resets only its own image's
// slice inside its command buffer, and results are read back for that slice
// on the NEXT use of the image (after its fence signaled), so queries are
// never reset while still in flight.
class GpuTimingSubsystem
{
  public:
    static constexpr uint32_t QUERIES_PER_PASS = 2;
    static constexpr uint32_t QUERIES_PER_IMAGE =
      QUERIES_PER_PASS * static_cast<uint32_t>(FrontendShared::GPU_TIMED_PASS_COUNT);

    // Rolling-window mean over the last WINDOW samples, used to smooth the
    // per-pass milliseconds shown in the GUI.
    struct GpuPassAverage
    {
        static constexpr uint32_t WINDOW = 30;
        std::array<float, WINDOW> samples{};
        uint32_t count{ 0 };
        uint32_t next{ 0 };
        float sum{ 0.0f };
        float add(float value)
        {
            if (count == WINDOW) {
                sum -= samples[next];
            } else {
                count++;
            }
            samples[next] = value;
            sum += value;
            next = (next + 1) % WINDOW;
            return sum / static_cast<float>(count);
        }
        void reset()
        {
            count = 0;
            next = 0;
            sum = 0.0f;
        }
    };

    // Creates (destroying any existing pool first) a query pool sized for
    // imageCount swapchain images, publishing support/reset state to
    // guiRendererSharedVars. On any creation failure, timestamps are marked
    // unsupported and the function returns early, exactly as the renderer did
    // inline.
    void create(VulkanDevice &device, uint32_t imageCount, FrontendShared::GUIRendererSharedVars &guiRendererSharedVars)
    {
        destroy(device);

        const uint32_t valid_bits = device.getGraphicsQueueTimestampValidBits();
        gpu_timestamp_period = device.getTimestampPeriod();
        gpu_timings_supported = (valid_bits != 0U) && (gpu_timestamp_period > 0.0F);

        guiRendererSharedVars.gpuTimings.supported = gpu_timings_supported;
        for (float &pass_ms : guiRendererSharedVars.gpuTimings.pass_ms) { pass_ms = -1.0F; }
        for (auto &average : gpu_pass_averages) { average.reset(); }

        if (!gpu_timings_supported) {
            spdlog::info(
              "GPU timestamps are not supported on the graphics queue family (timestampValidBits == 0); "
              "per-pass GPU timings disabled.");
            return;
        }

        gpu_timestamp_mask = (valid_bits >= 64U) ? ~0ULL : ((1ULL << valid_bits) - 1ULL);

        vk::QueryPoolCreateInfo query_pool_info{};
        query_pool_info.queryType = vk::QueryType::eTimestamp;
        query_pool_info.queryCount = QUERIES_PER_IMAGE * imageCount;

        auto pool_result = device.getLogicalDevice().createQueryPool(query_pool_info);
        if (pool_result.result != vk::Result::eSuccess) {
            spdlog::warn("Failed to create the GPU timing query pool (result {}); per-pass GPU timings disabled.",
              static_cast<int>(pool_result.result));
            gpu_timings_supported = false;
            guiRendererSharedVars.gpuTimings.supported = false;
            return;
        }
        gpu_timing_query_pool = pool_result.value;
        gpu_timing_pass_mask.assign(imageCount, 0U);
        gpu_timing_slice_recorded.assign(imageCount, false);
    }

    void destroy(VulkanDevice &device)
    {
        if (gpu_timing_query_pool) {
            device.getLogicalDevice().destroyQueryPool(gpu_timing_query_pool);
            gpu_timing_query_pool = nullptr;
        }
        gpu_timing_pass_mask.clear();
        gpu_timing_slice_recorded.clear();
    }

    // Reads back the previous results of imageIndex's slice (never waits) and
    // publishes smoothed per-pass milliseconds to the GUI shared vars.
    void readTimings(VulkanDevice &device,
      uint32_t imageIndex,
      FrontendShared::GUIRendererSharedVars &guiRendererSharedVars)
    {
        guiRendererSharedVars.gpuTimings.supported = gpu_timings_supported;

        if (!gpu_timings_supported || !gpu_timing_query_pool) { return; }
        // Freshly created pools hold queries in an undefined state; only read a
        // slice after it was reset and written at least once.
        if (imageIndex >= gpu_timing_slice_recorded.size() || !gpu_timing_slice_recorded[imageIndex]) { return; }

        // Two uint64 per query: [value, availability].
        std::array<uint64_t, static_cast<size_t>(QUERIES_PER_IMAGE) * 2U> query_data{};

        // Deliberately WITHOUT eWait: an unavailable result must be skipped, never
        // stalled on. eWithAvailability reports per-query availability.
        const vk::Result result = device.getLogicalDevice().getQueryPoolResults(gpu_timing_query_pool,
          imageIndex * QUERIES_PER_IMAGE,
          QUERIES_PER_IMAGE,
          sizeof(query_data),
          query_data.data(),
          2U * sizeof(uint64_t),
          vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
        if (result != vk::Result::eSuccess && result != vk::Result::eNotReady) { return; }

        const uint32_t recorded_passes = gpu_timing_pass_mask[imageIndex];
        constexpr double NANOSECONDS_PER_MILLISECOND = 1.0e6;
        bool frame_has_export_sample = false;

        for (int pass = 0; pass < FrontendShared::GPU_TIMED_PASS_COUNT; pass++) {
            if ((recorded_passes & (1U << static_cast<uint32_t>(pass))) == 0U) {
                // Pass not recorded in that frame (e.g. clouds/shadows disabled):
                // show it as inactive and drop stale history so a re-enabled pass
                // starts a fresh average.
                guiRendererSharedVars.gpuTimings.pass_ms[static_cast<size_t>(pass)] = -1.0F;
                gpu_pass_averages[static_cast<size_t>(pass)].reset();
                continue;
            }

            const size_t start_query = static_cast<size_t>(pass) * QUERIES_PER_PASS;
            const uint64_t start_value = query_data[start_query * 2U];
            const uint64_t start_available = query_data[(start_query * 2U) + 1U];
            const uint64_t end_value = query_data[(start_query + 1U) * 2U];
            const uint64_t end_available = query_data[((start_query + 1U) * 2U) + 1U];

            // Unavailable results are skipped (last smoothed value stays visible).
            if (start_available == 0U || end_available == 0U) { continue; }

            // Modular subtraction masked to timestampValidBits handles counter
            // wraparound on queue families with fewer than 64 valid bits.
            const uint64_t delta_ticks = (end_value - start_value) & gpu_timestamp_mask;
            const double pass_ms_raw = static_cast<double>(delta_ticks) * static_cast<double>(gpu_timestamp_period)
                                       / NANOSECONDS_PER_MILLISECOND;
            guiRendererSharedVars.gpuTimings.pass_ms[static_cast<size_t>(pass)] =
              gpu_pass_averages[static_cast<size_t>(pass)].add(static_cast<float>(pass_ms_raw));

            // The JSON export accumulates the RAW sample, not the smoothed value
            // the GUI shows - averaging averages would weight early frames by up
            // to WINDOW times. Warmup needs no extra handling here: the early
            // returns above already drop the frames without valid results
            // (observed on this machine: one readback per swapchain image is
            // skipped via gpu_timing_slice_recorded, i.e. the first 3 frames of a
            // triple-buffered run measure nothing, and the availability bits catch
            // any result the GPU has not finished).
            gpu_timing_export_sum_ms[static_cast<size_t>(pass)] += pass_ms_raw;
            gpu_timing_export_samples[static_cast<size_t>(pass)]++;
            frame_has_export_sample = true;
        }

        if (frame_has_export_sample) { gpu_timing_export_frames++; }
    }

    // Writes the per-pass averages as JSON when KATAGLYPHIS_GPU_TIMING_JSON
    // names a file. Called from cleanUp, while the accumulators and the
    // supported flag still describe the finished run.
    void writeJsonIfRequested()
    {
        const char *out_path = std::getenv("KATAGLYPHIS_GPU_TIMING_JSON");
        if (out_path == nullptr || *out_path == '\0') { return; }

        // The file is written even when timestamps are unsupported: a consumer
        // must be able to tell "this device cannot measure" (file present,
        // supported == false) from "the export never ran" (file missing).
        nlohmann::json dump;
        dump["frames_measured"] = gpu_timing_export_frames;
        dump["timestamps_supported"] = gpu_timings_supported;

        for (size_t pass = 0; pass < static_cast<size_t>(FrontendShared::GPU_TIMED_PASS_COUNT); pass++) {
            // A pass with no samples was never recorded (disabled feature or
            // unsupported timestamps); omitting it keeps 0.0 meaning "measured as
            // free" rather than "never measured".
            if (gpu_timing_export_samples[pass] == 0U) { continue; }
            dump["passes"][FrontendShared::GPU_TIMED_PASS_EXPORT_NAMES[pass]] =
              gpu_timing_export_sum_ms[pass] / static_cast<double>(gpu_timing_export_samples[pass]);
        }
        if (!dump.contains("passes")) { dump["passes"] = nlohmann::json::object(); }

        // ofstream reports failure via the stream state, never throws by default -
        // which is the only option anyway with exceptions disabled project-wide.
        std::ofstream file(out_path, std::ios::out | std::ios::trunc);
        if (!file.is_open()) {
            spdlog::error("GPU timing export: cannot open '{}' for writing; no JSON dumped.", out_path);
            return;
        }
        file << dump.dump(2) << '\n';
        file.close();
        if (file.fail()) {
            spdlog::error("GPU timing export: writing '{}' failed; the dump may be truncated.", out_path);
            return;
        }
        spdlog::info("GPU timing export: wrote {} ({} frames measured, timestamps supported: {}).",
          out_path,
          gpu_timing_export_frames,
          gpu_timings_supported);
    }

    [[nodiscard]] vk::QueryPool queryPool() const { return gpu_timing_query_pool; }
    [[nodiscard]] uint32_t queriesPerImage() const { return QUERIES_PER_IMAGE; }
    [[nodiscard]] bool isSupported() const { return gpu_timings_supported; }
    [[nodiscard]] uint64_t timestampMask() const { return gpu_timestamp_mask; }
    // Records the mask of passes written into imageIndex's slice this frame,
    // and marks the slice as safe to read back (freshly created pools hold
    // queries in an undefined state until reset+written at least once).
    void setPassRecordedMask(uint32_t imageIndex, uint32_t mask)
    {
        if (imageIndex >= gpu_timing_pass_mask.size()) { return; }
        gpu_timing_pass_mask[imageIndex] = mask;
        gpu_timing_slice_recorded[imageIndex] = true;
    }

  private:
    bool gpu_timings_supported{ false };
    float gpu_timestamp_period{ 0.0f };
    uint64_t gpu_timestamp_mask{ ~0ULL };
    vk::QueryPool gpu_timing_query_pool{};
    // Per swapchain image: bitmask of passes actually recorded last time.
    std::vector<uint32_t> gpu_timing_pass_mask;
    // Per swapchain image: whether the slice was ever reset+written (freshly
    // created pools contain queries in an undefined state that must not be
    // read).
    std::vector<bool> gpu_timing_slice_recorded;

    std::array<GpuPassAverage, FrontendShared::GPU_TIMED_PASS_COUNT> gpu_pass_averages{};

    // -- headless GPU-timing export (KATAGLYPHIS_GPU_TIMING_JSON)
    // Unsmoothed per-pass totals over the renderer's whole lifetime, fed by
    // readTimings. Kept separate from GpuPassAverage on purpose: the GUI
    // wants a short smoothed window, the export wants the true mean over every
    // measured frame. Deliberately not reset on swapchain recreation - the
    // queries restart, the run's statistics do not.
    std::array<double, FrontendShared::GPU_TIMED_PASS_COUNT> gpu_timing_export_sum_ms{};
    std::array<uint64_t, FrontendShared::GPU_TIMED_PASS_COUNT> gpu_timing_export_samples{};
    uint64_t gpu_timing_export_frames{ 0 };
};
}// namespace Kataglyphis
