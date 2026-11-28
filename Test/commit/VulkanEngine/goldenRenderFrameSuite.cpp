// Goldens: a frame draws, its stages run in order, timing, culling, framing and presentation hold, sync stays clean.

#include "GoldenRenderHarness.hpp"

import kataglyphis.vulkan.renderer;

using namespace Kataglyphis::TestSupport::GoldenRender;

namespace {

// The renderer reads KATAGLYPHIS_GPU_TIMING_JSON in cleanUp, so it must outlive the harness.
class ScopedGpuTimingJsonPath
{
  public:
    explicit ScopedGpuTimingJsonPath(const std::string &path) : env_("KATAGLYPHIS_GPU_TIMING_JSON", path.c_str()) {}

  private:
    ScopedEnvVar env_;
};

}// namespace

// Cheapest guard against the whole render graph silently producing nothing.
TEST(GoldenRender, RendersNonBlankFrame)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &scene_vars = harness.settings.scene;
    harness.useForwardRaster();
    scene_vars.shadows_enabled = true;

    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> frame = harness.capture_frame(width, height);

    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
    ASSERT_FALSE(frame.empty()) << "Frame capture returned no pixels.";
    EXPECT_GT(width, 0U);
    EXPECT_GT(height, 0U);
    ASSERT_EQ(frame.size(), static_cast<size_t>(width) * static_cast<size_t>(height) * 4U)
      << "Captured buffer size does not match the reported extent.";

    const double stddev = luminance_stddev(frame);
    const double lit_fraction = fraction_above(frame, 8.0);
    const size_t buckets = distinct_luminance_buckets(frame);

    EXPECT_GT(stddev, 2.0) << "Frame is essentially uniform (luminance stddev " << stddev
                           << "); the render graph likely produced a blank image.";
    EXPECT_GT(lit_fraction, 0.05) << "Only " << (lit_fraction * 100.0)
                                  << "% of pixels are non-black; the frame looks empty.";
    EXPECT_GT(buckets, 8U) << "Only " << buckets
                           << " distinct luminance levels present; the frame looks like a flat fill.";
}

// The registry replaced a hard-wired record_commands; a frame must still record the passes in its old order.
TEST(GoldenRender, BuiltInStagesRecordInTheFrameOrder)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    auto &scene_vars = harness.settings.scene;
    harness.useForwardRaster();
    scene_vars.shadows_enabled = true;
    scene_vars.clouds_enabled = true;
    // The default half-resolution march records the upsample between the march and post; the sky draws in main.
    harness.render_frames(1);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    EXPECT_EQ(harness.renderer->renderStageRegistry().lastRecorded(),
      (std::vector<std::string>{ "shadow_cascades", "main", "clouds", "cloud_upsample", "exposure_reduce", "post" }));

    // A full-resolution march fills the full-size target itself.
    scene_vars.cloud_half_resolution = false;
    harness.render_frame();
    EXPECT_EQ(harness.renderer->renderStageRegistry().lastRecorded(),
      (std::vector<std::string>{ "shadow_cascades", "main", "clouds", "exposure_reduce", "post" }));

    scene_vars.shadows_enabled = false;
    scene_vars.clouds_enabled = false;
    harness.render_frame();
    EXPECT_EQ(harness.renderer->renderStageRegistry().lastRecorded(),
      (std::vector<std::string>{ "main", "exposure_reduce", "post" }));

    // Auto exposure meters the HDR target before the reduce.
    harness.settings.renderer.auto_exposure = true;
    harness.render_frame();
    EXPECT_EQ(harness.renderer->renderStageRegistry().lastRecorded(),
      (std::vector<std::string>{ "main", "histogram", "exposure_reduce", "post" }));

    // Bloom thresholds the exposed frame, so it follows the reduce; post adds it.
    harness.settings.renderer.bloom_strength = 0.6F;
    harness.render_frame();
    EXPECT_EQ(harness.renderer->renderStageRegistry().lastRecorded(),
      (std::vector<std::string>{ "main", "histogram", "exposure_reduce", "bloom", "post" }));
}

// What the registry is for: a pass added from outside VulkanRenderer records, resizes and leaves again.
TEST(GoldenRender, StageAddedFromOutsideTheRendererDrawsAfterPost)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    const ScopedValidationErrorCounter validation;
    harness.useForwardRaster();
    harness.render_frames(WARMUP_FRAMES);

    int resizes = 0;
    vk::Extent2D resized_to{};
    Kataglyphis::RenderStage overlay;
    overlay.name = "test_overlay";
    overlay.order = Kataglyphis::RenderStageOrder::Post + 50;
    // Clears the presentable image post left behind to magenta, then hands it back presentable.
    overlay.record = [](const Kataglyphis::RenderStageFrame &frame) {
        Kataglyphis::recordImageBarrier(frame.commandBuffer,
          Kataglyphis::buildImageMemoryBarrier(frame.swapchainImage,
            vk::ImageLayout::ePresentSrcKHR,
            vk::ImageLayout::eColorAttachmentOptimal,
            vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentWrite,
            vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentWrite));
        const std::array<vk::RenderingAttachmentInfo, 1> colour = { Kataglyphis::buildRenderingAttachmentInfo(
          frame.swapchainImageView,
          vk::ImageLayout::eColorAttachmentOptimal,
          vk::AttachmentLoadOp::eClear,
          vk::AttachmentStoreOp::eStore,
          vk::ClearValue{ vk::ClearColorValue{ std::array{ 1.0F, 0.0F, 1.0F, 1.0F } } }) };
        frame.commandBuffer.beginRendering(Kataglyphis::buildRenderingInfo(frame.extent, colour, nullptr));
        frame.commandBuffer.endRendering();
        Kataglyphis::recordImageBarrier(frame.commandBuffer,
          Kataglyphis::buildImageMemoryBarrier(frame.swapchainImage,
            vk::ImageLayout::eColorAttachmentOptimal,
            vk::ImageLayout::ePresentSrcKHR,
            vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentWrite,
            vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eNone));
    };
    overlay.resize = [&resizes, &resized_to](const Kataglyphis::RenderStageResize &resize) {
        ++resizes;
        resized_to = resize.extent;
    };
    ASSERT_EQ(harness.renderer->addRenderStage(std::move(overlay)), Kataglyphis::RenderStageRegistry::AddResult::Added);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> covered = harness.capture_frame(width, height);
    ASSERT_FALSE(covered.empty()) << "Frame capture returned no pixels.";
    const std::vector<std::string> &recorded = harness.renderer->renderStageRegistry().lastRecorded();
    ASSERT_GE(recorded.size(), 2U);
    EXPECT_EQ(recorded[recorded.size() - 2U], "post");
    EXPECT_EQ(recorded.back(), "test_overlay");

    size_t magenta = 0;
    for (size_t pixel = 0; pixel < covered.size() / 4U; ++pixel) {
        const uint8_t *rgba = &covered[pixel * 4U];
        if (rgba[0] >= 253U && rgba[1] <= 2U && rgba[2] >= 253U) { ++magenta; }
    }
    EXPECT_EQ(magenta, covered.size() / 4U) << "the added stage's clear did not cover the frame post drew";

    harness.renderer->recreateSwapChain();
    EXPECT_EQ(resizes, 1);
    EXPECT_EQ(resized_to, (vk::Extent2D{ width, height }));

    ASSERT_TRUE(harness.renderer->removeRenderStage("test_overlay"));
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> scene = harness.capture_frame(width, height);
    EXPECT_GT(luminance_stddev(scene), 2.0) << "the scene did not come back once the stage was removed";
    EXPECT_EQ(validation.errorCount(), 0) << "the added stage's frames raised validation errors";
}

// Disabled: writes PNGs and asserts nothing; see docs/source/gpu-golden-testing.md § Writing a new golden test.
TEST(GoldenRender, DISABLED_DumpsFrameToPng)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    // A screenshot of the editor itself, for looking at the GUI; the pixel comparisons below then differ.
    if (Kataglyphis::environmentFlag("KATAGLYPHIS_FRAME_DUMP_WITH_UI")) { harness.gui->setOverlayVisible(true); }
    harness.settings.scene.shadows_enabled = true;
    // Must match ShadowsDarkenSomePixels' configuration, or the two cannot be compared.
    harness.settings.scene.cascaded_shadow_intensity = 1.0F;

    harness.render_frames(WARMUP_FRAMES);
    harness.render_frames(SETTLE_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> frame = harness.capture_frame(width, height);
    ASSERT_FALSE(frame.empty()) << "Capture returned no pixels.";

    const std::string base = Kataglyphis::environmentVariable("KATAGLYPHIS_FRAME_DUMP").value_or("frame-dump");
    const auto write = [&](const std::string &suffix, const std::vector<uint8_t> &pixels) {
        const std::string path = base + suffix + ".png";
        ASSERT_NE(stbi_write_png(path.c_str(),
                    static_cast<int>(width),
                    static_cast<int>(height),
                    4,
                    pixels.data(),
                    static_cast<int>(width) * 4),
          0)
          << "Failed to write " << path;
        std::cout << "[  INFO ] wrote " << path << " (" << width << "x" << height << ")\n";
    };

    write("-shadows-on", frame);

    // ShadowsDarkenSomePixels' exact sequence, with the PNG of the very bytes each mean was taken over.
    auto &scene_vars = harness.settings.scene;

    scene_vars.cascaded_shadow_intensity = 0.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> golden_order_off = harness.capture_frame(width, height);

    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> golden_order_noise = harness.capture_frame(width, height);

    scene_vars.cascaded_shadow_intensity = 1.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> golden_order_on = harness.capture_frame(width, height);

    ASSERT_EQ(golden_order_off.size(), golden_order_on.size());
    GTEST_LOG_(INFO) << "golden-order means: intensity 0.0 = " << mean_luminance(golden_order_off)
                     << ", noise reference (same 0.0 state) = " << mean_luminance(golden_order_noise)
                     << ", intensity 1.0 = " << mean_luminance(golden_order_on);

    write("-golden-order-off", golden_order_off);
    write("-golden-order-on", golden_order_on);

    // One PCF tap makes the term binary, separating a lacy occluder's weak shadow from a defect.
    const int restore_pcf = scene_vars.pcf_radius;
    scene_vars.pcf_radius = 0;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> single_tap_on = harness.capture_frame(width, height);
    scene_vars.cascaded_shadow_intensity = 0.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> single_tap_off = harness.capture_frame(width, height);
    scene_vars.pcf_radius = restore_pcf;
    scene_vars.cascaded_shadow_intensity = 1.0F;

    GTEST_LOG_(INFO) << "single-tap PCF means: intensity 1.0 = " << mean_luminance(single_tap_on)
                     << ", intensity 0.0 = " << mean_luminance(single_tap_off);
    write("-singletap-on", single_tap_on);

    std::vector<uint8_t> single_tap_delta(single_tap_on.size(), 255);
    for (size_t i = 0; i + 3 < single_tap_on.size(); i += 4) {
        for (size_t channel = 0; channel < 3; ++channel) {
            const int d = static_cast<int>(single_tap_off[i + channel]) - static_cast<int>(single_tap_on[i + channel]);
            single_tap_delta[i + channel] = static_cast<uint8_t>(255 - std::clamp(d * 4, 0, 255));
        }
    }
    write("-singletap-delta", single_tap_delta);

    // A settle far past SETTLE_FRAMES rules out slow GUI-to-UBO propagation.
    scene_vars.cascaded_shadow_intensity = 0.0F;
    harness.render_frames(20);
    uint32_t off_width = 0;
    uint32_t off_height = 0;
    const std::vector<uint8_t> unshadowed = harness.capture_frame(off_width, off_height);
    ASSERT_EQ(unshadowed.size(), frame.size());

    GTEST_LOG_(INFO) << "dump-order means: intensity 1.0 (first capture) = " << mean_luminance(frame)
                     << ", intensity 0.0 (after 20 frames) = " << mean_luminance(unshadowed);

    write("-shadows-off", unshadowed);

    std::vector<uint8_t> difference(frame.size(), 255);
    for (size_t i = 0; i + 3 < frame.size(); i += 4) {
        for (size_t channel = 0; channel < 3; ++channel) {
            const int delta = static_cast<int>(unshadowed[i + channel]) - static_cast<int>(frame[i + channel]);
            // Inverted 32x gain, because this scene's shadow is genuinely faint.
            difference[i + channel] = static_cast<uint8_t>(255 - std::clamp(delta * 32, 0, 255));
        }
    }
    write("-shadow-delta", difference);

    // Delta of the golden-order pair too, at the same gain.
    std::vector<uint8_t> golden_delta(golden_order_on.size(), 255);
    for (size_t i = 0; i + 3 < golden_order_on.size(); i += 4) {
        for (size_t channel = 0; channel < 3; ++channel) {
            const int d =
              static_cast<int>(golden_order_off[i + channel]) - static_cast<int>(golden_order_on[i + channel]);
            golden_delta[i + channel] = static_cast<uint8_t>(255 - std::clamp(d * 32, 0, 255));
        }
    }
    write("-golden-order-delta", golden_delta);
}

// Mean row of the pixels at x >= 68% where `after` moves any channel more than 12 levels from `before`.
static std::optional<double> changed_centroid_row(const std::vector<uint8_t> &before,
  const std::vector<uint8_t> &after,
  uint32_t w,
  uint32_t h,
  size_t &count)
{
    double row_sum = 0.0;
    count = 0;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = (w * 68U) / 100U; x < w; ++x) {
            const size_t base = (static_cast<size_t>(y) * w + x) * 4U;
            for (size_t c = 0; c < 3U; ++c) {
                if (std::abs(static_cast<int>(after[base + c]) - static_cast<int>(before[base + c])) > 12) {
                    row_sum += static_cast<double>(y);
                    ++count;
                    break;
                }
            }
        }
    }
    return count > 200U ? std::optional<double>(row_sum / static_cast<double>(count)) : std::nullopt;
}

// The camera aims at y = 4 where the red/green cards stand: y = 6.5 belongs in the upper half, 1.5 in the lower.
TEST(GoldenRender, WorldUpIsScreenUp)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    struct Mode
    {
        const char *name;
        bool raytracing;
        RasterizationMode raster;
    };
    std::vector<Mode> modes{ { "forward", false, RasterizationMode::Forward },
        { "deferred", false, RasterizationMode::Deferred } };
    if (harness.renderer->supportsHardwareRaytracing()) {
        modes.push_back({ "raytracing", true, RasterizationMode::Forward });
    }

    harness.useForwardRaster();
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";
    // A card's shadow on the ground would pull its changed-pixel centroid down.
    harness.settings.scene.shadows_enabled = false;

    uint32_t width = 0;
    uint32_t height = 0;
    const auto capture_all = [&] {
        std::vector<std::vector<uint8_t>> frames;
        for (const Mode &mode : modes) {
            auto &renderer_vars = harness.settings.renderer;
            renderer_vars.raytracing = mode.raytracing;
            renderer_vars.pathTracing = false;
            renderer_vars.rasterizationMode = mode.raster;
            harness.render_frames(SETTLE_FRAMES);
            frames.push_back(harness.capture_frame(width, height));
        }
        return frames;
    };
    const auto add_card = [&](float y) {
        const glm::mat4 placement =
          glm::translate(glm::mat4(1.0F), glm::vec3(5.0F, y, 15.0F)) * glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
        return harness.renderer->addModel(TWO_PRIMITIVE_MODEL, placement).has_value();
    };

    const auto base = capture_all();
    ASSERT_TRUE(add_card(6.5F)) << "adding the high card failed";
    const auto high = capture_all();
    ASSERT_TRUE(add_card(1.5F)) << "adding the low card failed";
    const auto low = capture_all();
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    const double half = static_cast<double>(height) / 2.0;
    for (size_t i = 0; i < modes.size(); ++i) {
        ASSERT_FALSE(base[i].empty() || high[i].empty() || low[i].empty()) << modes[i].name;
        size_t high_count = 0;
        size_t low_count = 0;
        const auto high_row = changed_centroid_row(base[i], high[i], width, height, high_count);
        const auto low_row = changed_centroid_row(high[i], low[i], width, height, low_count);
        GTEST_LOG_(INFO) << modes[i].name << ": high card " << high_count << " px at mean row "
                         << high_row.value_or(-1.0) << ", low card " << low_count << " px at mean row "
                         << low_row.value_or(-1.0) << " of " << height;
        ASSERT_TRUE(high_row.has_value() && low_row.has_value())
          << modes[i].name << ": a card did not brighten the right of the frame - framing changed";
        EXPECT_LT(*high_row, half) << modes[i].name << ": the card above the aim point is in the lower half";
        EXPECT_GT(*low_row, half) << modes[i].name << ": the card below the aim point is in the upper half";
    }
}

// Culling that works has no visual signature, so the wiring is checked through drawn/considered counters.
TEST(GoldenRender, FrustumCullingDropsOffscreenMeshesOnly)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    auto &renderer_vars = harness.useForwardRaster();
    renderer_vars.frustum_culling_enabled = true;

    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    // The default debug camera frames the scene, so everything is visible.
    const unsigned int considered = harness.feedback.visibility.meshes_total;
    ASSERT_GT(considered, 0U) << "the renderer reported considering no meshes at all; "
                                 "the visibility counters are not being written";
    EXPECT_EQ(harness.feedback.visibility.meshes_drawn, considered)
      << "culling dropped geometry the camera is looking straight at";

    // Far past the far plane, with the scene behind the camera.
    harness.camera->set_camera_position(glm::vec3(0.0F, 0.0F, 5000.0F));
    harness.render_frames(SETTLE_FRAMES);

    EXPECT_EQ(harness.feedback.visibility.meshes_total, considered)
      << "the number of meshes considered must not change when the camera moves";
    EXPECT_EQ(harness.feedback.visibility.meshes_drawn, 0U)
      << "a scene entirely outside the view should be culled completely, but "
      << harness.feedback.visibility.meshes_drawn << " mesh(es) were still drawn";

    // Separates "culling works" from "the renderer stopped drawing".
    renderer_vars.frustum_culling_enabled = false;
    harness.render_frames(SETTLE_FRAMES);
    EXPECT_EQ(harness.feedback.visibility.meshes_drawn, considered)
      << "with culling disabled every mesh must be submitted regardless of the camera";

    ASSERT_FALSE(harness.renderer->hasDeviceLost());
}

// Covers the whole loop: env var, accumulation over real frames, the write in cleanUp, and the schema.
TEST(GoldenRender, GpuTimingJsonDumpIsWrittenAndSane)
{
    SKIP_WITHOUT_GPU();

    const std::filesystem::path json_path =
      std::filesystem::temp_directory_path() / "kataglyphis-gpu-timing-dump-test.json";
    std::error_code filesystem_error;
    std::filesystem::remove(json_path, filesystem_error);
    ASSERT_FALSE(std::filesystem::exists(json_path, filesystem_error))
      << "Stale dump at " << json_path << " could not be removed; the test would read a lie.";

    const ScopedGpuTimingJsonPath scoped_path(json_path.string());
    constexpr int LOADED_FRAMES = 30;

    {
        EngineHarness harness;
        harness.useForwardRaster();
        harness.settings.scene.shadows_enabled = true;

        // Fresh query pools are unreadable until recorded once, so the first readbacks measure nothing.
        harness.render_frames(LOADED_FRAMES);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while rendering.";
    }// the renderer's destructor writes the dump

    ASSERT_TRUE(std::filesystem::exists(json_path, filesystem_error))
      << "Renderer teardown did not write " << json_path;

    std::ifstream file(json_path);
    ASSERT_TRUE(file.is_open()) << "Cannot read back " << json_path;
    std::stringstream content;
    content << file.rdbuf();

    // Non-throwing parse: exceptions are disabled project-wide.
    const nlohmann::json dump = nlohmann::json::parse(content.str(), nullptr, false);
    ASSERT_FALSE(dump.is_discarded()) << "GPU timing dump is not valid JSON: " << content.str();

    ASSERT_TRUE(dump.contains("frames_measured")) << dump.dump(2);
    ASSERT_TRUE(dump.contains("timestamps_supported")) << dump.dump(2);
    ASSERT_TRUE(dump.contains("passes")) << dump.dump(2);
    ASSERT_TRUE(dump["passes"].is_object()) << dump.dump(2);

    if (!dump["timestamps_supported"].get<bool>()) {
        // A parsing file is what separates "cannot measure" from "export broken" here.
        EXPECT_EQ(dump["frames_measured"].get<std::uint64_t>(), 0U)
          << "A device without timestamps cannot have measured frames.";
        EXPECT_TRUE(dump["passes"].empty()) << dump.dump(2);
        GTEST_SKIP() << "GPU timestamps unsupported on this device; average checks not possible.";
    }

    EXPECT_GT(dump["frames_measured"].get<std::uint64_t>(), 0U)
      << "Timestamps are supported but no frame produced a valid sample: " << dump.dump(2);
    // About 240 sky-only frames before Dinosaurs arrived once outweighed its six, so Forward read 60 times too cheap.
    EXPECT_LE(dump["frames_measured"].get<std::uint64_t>(), static_cast<std::uint64_t>(LOADED_FRAMES))
      << "the dump counted frames drawn before the model arrived, or the load frame: " << dump.dump(2);

    // These are bracketed every frame, so absence means broken accumulation, not a disabled feature.
    for (const char *always_recorded : { "Main", "Forward", "Post" }) {
        EXPECT_TRUE(dump["passes"].contains(always_recorded))
          << "Pass '" << always_recorded << "' is recorded every frame but missing: " << dump.dump(2);
    }
    // The sky draws inside Forward, as Rust's does, so no scope of its own times it.
    EXPECT_FALSE(dump["passes"].contains("Sky")) << dump.dump(2);
    // A forward frame draws inside Main through Forward alone; the budget script sums only what nests nowhere.
    ASSERT_TRUE(dump.contains("nested_in") && dump["nested_in"].is_object()) << dump.dump(2);
    EXPECT_EQ(dump["nested_in"].value("Forward", ""), "Main") << dump.dump(2);
    for (const char *not_drawn : { "GBuffer", "DeferredLighting" }) {
        EXPECT_FALSE(dump["passes"].contains(not_drawn)) << "a forward run timed " << not_drawn << ": " << dump.dump(2);
    }
    if (dump["passes"].contains("Main") && dump["passes"].contains("Forward")) {
        EXPECT_LE(dump["passes"]["Forward"].get<double>(), dump["passes"]["Main"].get<double>() * 1.01)
          << "Forward is stamped inside Main, so it cannot take longer: " << dump.dump(2);
    }

    for (const auto &[pass_name, average_ms] : dump["passes"].items()) {
        ASSERT_TRUE(average_ms.is_number()) << "Pass '" << pass_name << "' is not a number: " << dump.dump(2);
        const double value = average_ms.get<double>();
        EXPECT_TRUE(std::isfinite(value)) << "Pass '" << pass_name << "' average is not finite.";
        EXPECT_GE(value, 0.0) << "Pass '" << pass_name << "' average is negative.";
    }
}

// Main times the scope that drew inside it: Forward, or the deferred pair stamped around the G-buffer pass.
TEST(GoldenRender, GpuTimingSplitsMainIntoTheScopeThatDrew)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    const auto &timings = harness.feedback.gpuTimings;
    auto &renderer_vars = harness.useForwardRaster();
    // A frame reads back what its swapchain image drew last, so past the image count every reading is this mode's.
    constexpr int READBACK_FRAMES = 8;
    harness.render_frames(WARMUP_FRAMES + READBACK_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    if (!timings.supported) { GTEST_SKIP() << "GPU timestamps unsupported on this device."; }

    const auto ms = [&timings](GpuTimedPass pass) { return timings.pass_ms[static_cast<size_t>(pass)]; };
    EXPECT_GE(ms(GpuTimedPass::Main), 0.0F);
    EXPECT_GE(ms(GpuTimedPass::Forward), 0.0F) << "a forward frame must time Forward";
    const float forward_ms = ms(GpuTimedPass::Forward);
    EXPECT_LT(ms(GpuTimedPass::GBuffer), 0.0F) << "a forward frame timed a G-buffer pass";
    EXPECT_LT(ms(GpuTimedPass::DeferredLighting), 0.0F) << "a forward frame timed a lighting pass";

    renderer_vars.rasterizationMode = RasterizationMode::Deferred;
    harness.render_frames(READBACK_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    EXPECT_GE(ms(GpuTimedPass::Main), 0.0F);
    EXPECT_GE(ms(GpuTimedPass::GBuffer), 0.0F) << "a deferred frame must time its G-buffer pass";
    EXPECT_GE(ms(GpuTimedPass::DeferredLighting), 0.0F) << "a deferred frame must time its lighting pass";
    EXPECT_LT(ms(GpuTimedPass::Forward), 0.0F) << "a deferred frame timed Forward";
    EXPECT_LT(ms(GpuTimedPass::RayTrace), 0.0F);
    EXPECT_LT(ms(GpuTimedPass::PathTrace), 0.0F);
    // Both averages start at the model's arrival or the switch; llvmpipe compiles the deferred pair on first use.
    GTEST_LOG_(INFO) << "forward " << forward_ms << " ms; deferred G-buffer " << ms(GpuTimedPass::GBuffer)
                     << " ms, lighting " << ms(GpuTimedPass::DeferredLighting)
                     << " ms; Main's average still holds forward frames";
}

// The app presents in Fifo by default; the harness pins VSync off, and a toggle must rebuild the chain both ways.
TEST(GoldenRender, VsyncTogglesBetweenFifoAndAnUncappedMode)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    const std::vector<vk::PresentModeKHR> offered = harness.renderer->offeredPresentModes();
    const auto offers = [&offered](vk::PresentModeKHR mode) {
        return std::find(offered.begin(), offered.end(), mode) != offered.end();
    };
    const bool uncapped_offered = offers(vk::PresentModeKHR::eMailbox) || offers(vk::PresentModeKHR::eImmediate);
    const auto expect_uncapped = [&](const char *when) {
        const vk::PresentModeKHR mode = harness.renderer->presentMode();
        if (uncapped_offered) {
            EXPECT_TRUE(mode == vk::PresentModeKHR::eMailbox || mode == vk::PresentModeKHR::eImmediate)
              << when << ": VSync off presented in " << vk::to_string(mode);
        } else {
            EXPECT_EQ(mode, vk::PresentModeKHR::eFifo) << when << ": no uncapped mode offered, so Fifo";
        }
    };
    std::string offered_names;
    for (const vk::PresentModeKHR mode : offered) { offered_names += " " + vk::to_string(mode); }
    GTEST_LOG_(INFO) << "the surface offers" << offered_names << "; VSync off took "
                     << vk::to_string(harness.renderer->presentMode());
    ASSERT_TRUE(Kataglyphis::RenderSettings{}.renderer.vsync) << "VSync must be on by default, as in Rust";
    ASSERT_FALSE(harness.settings.renderer.vsync) << "the goldens run with VSync off";
    expect_uncapped("the harness's first chain");

    harness.render_frames(WARMUP_FRAMES);
    harness.settings.renderer.vsync = true;
    harness.render_frame();
    EXPECT_EQ(harness.renderer->presentMode(), vk::PresentModeKHR::eFifo) << "VSync on did not rebuild the chain";
    uint32_t width = 0;
    uint32_t height = 0;
    EXPECT_FALSE(harness.capture_frame(width, height).empty()) << "no frame after turning VSync on";

    harness.settings.renderer.vsync = false;
    harness.render_frame();
    expect_uncapped("after turning VSync off again");
    EXPECT_FALSE(harness.capture_frame(width, height).empty()) << "no frame after turning VSync off";
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
}

// The layer reads them at instance creation, so one harness gets sync validation whatever the lane's settings say.
class ScopedSyncValidation
{
  public:
    ScopedSyncValidation() { setAll("1"); }
    ScopedSyncValidation(const ScopedSyncValidation &) = delete;
    ScopedSyncValidation &operator=(const ScopedSyncValidation &) = delete;
    ~ScopedSyncValidation() { setAll(""); }

  private:
    // Both extras default off and hide a whole class: a load op after another pass's store, and a shader's reads.
    static constexpr std::array<const char *, 3> kSettings = { "VK_KHRONOS_VALIDATION_VALIDATE_SYNC",
        "VK_KHRONOS_VALIDATION_SYNCVAL_LOAD_OP_AFTER_STORE_OP_VALIDATION",
        "VK_KHRONOS_VALIDATION_SYNCVAL_SHADER_ACCESSES_HEURISTIC" };

    static void setAll(const char *value)
    {
        for (const char *name : kSettings) { set(name, value); }
    }

    static void set(const char *name, const char *value)
    {
#ifdef _WIN32
        std::ignore = _putenv_s(name, value);
#else
        if (*value == '\0') {
            std::ignore = unsetenv(name);
        } else {
            std::ignore = setenv(name, value, 1);
        }
#endif
    }
};

// Both wrote without a barrier before: the deferred pass's G-buffer stores and back-to-back BLAS builds on one scratch.
TEST(GoldenRender, DeferredFramesAndBlasRebuildsAreFreeOfSyncHazards)
{
    SKIP_WITHOUT_GPU();

    ScopedSyncValidation sync_validation;
    EngineHarness harness;
    harness.useForwardRaster().rasterizationMode = RasterizationMode::Deferred;
    // From the first frame: the layer reports one message id ten times, then goes quiet, so warmup would spend them.
    ScopedValidationErrorCounter validation_errors;
    harness.render_frames(WARMUP_FRAMES);
    // A second model rebuilds every BLAS in one command buffer through one scratch buffer.
    const auto added = harness.renderer->addModel(UV_TRANSFORM_MODEL, panel_free_card_placement());
    ASSERT_TRUE(added.has_value()) << "adding the second model failed";
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost in the deferred path.";

    EXPECT_EQ(validation_errors.errorCount(), 0)
      << "synchronization validation logged an error - SYNC-HAZARD-WRITE-AFTER-WRITE if a deferred G-buffer store "
         "or a BLAS build's scratch write lost its barrier (DeferredRasterizer.cpp, ASManager.cpp)";
}

// Default sync validation misses both edges: the trace into the sky's blend, and the clouds' compute output into post.
TEST(GoldenRender, CloudCompositeAndTracedSkyAreFreeOfSyncHazards)
{
    SKIP_WITHOUT_GPU();

    ScopedSyncValidation sync_validation;
    EngineHarness harness;
    harness.useForwardRaster();
    harness.settings.scene.clouds_enabled = true;
    // From the first frame: the layer reports one message id ten times, then goes quiet, so warmup would spend them.
    ScopedValidationErrorCounter validation_errors;
    harness.render_frames(WARMUP_FRAMES + SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost compositing the clouds.";
    if (harness.renderer->supportsHardwareRaytracing()) {
        harness.settings.renderer.raytracing = true;
        harness.render_frames(SETTLE_FRAMES);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost filling the traced frame's sky.";
    }

    EXPECT_EQ(validation_errors.errorCount(), 0)
      << "synchronization validation logged an error - SYNC-HAZARD-READ-AFTER-WRITE if the traced sky lost its barrier "
         "after the trace (SkyBox.cpp) or post after the clouds' dispatch (VulkanRenderer.cpp)";
}

// Every frame in flight shares the scene targets, so each first write must wait out the last frame's reads of them.
TEST(GoldenRender, SharedSceneTargetsAreFreeOfCrossFrameHazards)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    ScopedSyncValidation sync_validation;
    EngineHarness harness;
    auto &renderer_vars = harness.useForwardRaster();
    // Every reader of the HDR target: the histogram's compute pass, bloom's bright pass, post, and FXAA after it.
    renderer_vars.auto_exposure = true;
    renderer_vars.bloom_strength = 0.6F;
    renderer_vars.ssao_strength = 0.7F;
    renderer_vars.fxaaMode = FxaaMode::Always;
    renderer_vars.pathTracingSamplesPerPixel = 1;
    renderer_vars.pathTracingMaxBounces = 1;
    // From the first frame: the layer reports one message id ten times, then goes quiet, so warmup would spend them.
    ScopedValidationErrorCounter validation_errors;
    harness.render_frames(WARMUP_FRAMES);
    // Back to back, each mode's first writer follows frames of its own and, on the switch, another mode's.
    renderer_vars.rasterizationMode = RasterizationMode::Deferred;
    harness.render_frames(SETTLE_FRAMES);
    if (harness.renderer->supportsHardwareRaytracing()) {
        renderer_vars.raytracing = true;
        harness.render_frames(SETTLE_FRAMES);
        renderer_vars.raytracing = false;
        renderer_vars.pathTracing = true;
        harness.render_frames(SETTLE_FRAMES);
        renderer_vars.pathTracing = false;
    }
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    EXPECT_EQ(validation_errors.errorCount(), 0)
      << "synchronization validation logged an error - SYNC-HAZARD-WRITE-AFTER-READ if a pass's first barrier on the "
         "shared HDR target, G-buffer or FXAA intermediate no longer waits out the previous frame's reads "
         "(Rasterizer.cpp, DeferredRasterizer.cpp, Raytracing.cpp, PathTracing.cpp, PostStage.cpp)";
}

// Nothing else recreates the swapchain, so a dangling fence or stale framebuffer would pass every other test.
TEST(GoldenRender, SwapchainRecreationKeepsRendering)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();

    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> before = harness.capture_frame(width, height);
    ASSERT_FALSE(before.empty());
    const double before_luma = mean_luminance(before);

    // The window does not resize, so the recreated frame must be equivalent.
    harness.renderer->recreateSwapChain();
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during swapchain recreation.";

    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost after swapchain recreation.";

    uint32_t width2 = 0;
    uint32_t height2 = 0;
    const std::vector<uint8_t> after = harness.capture_frame(width2, height2);
    ASSERT_FALSE(after.empty());
    EXPECT_EQ(width, width2);
    EXPECT_EQ(height, height2);

    // The tolerance absorbs overlay jitter; a black or half-torn frame swings far beyond it.
    const double after_luma = mean_luminance(after);
    EXPECT_NEAR(after_luma, before_luma, 20.0)
      << "Mean luminance changed too much across swapchain recreation (before=" << before_luma
      << ", after=" << after_luma << ") - the recreated swapchain is not rendering the same scene.";
}

// A scene with its own camera starts at it (owner rule), and its credit reaches the About window's feedback.
TEST(GoldenRender, ASceneWithItsOwnCameraIsDrawnFromIt)
{
    SKIP_WITHOUT_GPU();

    // An unlit red card at x = 100, which the golden view never sees, and a narrow camera ten units in front of it.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kat_golden_scene_camera";
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
    std::filesystem::create_directories(dir);
    const std::filesystem::path model = dir / "card_camera.gltf";
    {
        std::ofstream out(model, std::ios::binary);
        out << R"GLTF({
          "asset": { "version": "2.0", "copyright": "Card by Somebody, CC0" },
          "extensionsUsed": [ "KHR_materials_unlit" ],
          "scene": 0,
          "scenes": [ { "nodes": [ 0, 1 ], "extras": { "active_camera": "Front" } } ],
          "nodes": [ { "mesh": 0 }, { "name": "Front", "camera": 0, "translation": [101, 1, 10] } ],
          "cameras": [ { "type": "perspective", "perspective": { "yfov": 0.4, "znear": 0.1 } } ],
          "materials": [ { "doubleSided": true, "extensions": { "KHR_materials_unlit": {} },
                           "pbrMetallicRoughness": { "baseColorFactor": [1, 0, 0, 1] } } ],
          "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 }, "material": 0 } ] } ],
          "accessors": [ { "componentType": 5126, "count": 6, "type": "VEC3",
                           "min": [100, 0, 0], "max": [102, 2, 0], "bufferView": 0 } ],
          "bufferViews": [ { "buffer": 0, "byteLength": 72 } ],
          "buffers": [ { "byteLength": 72, "uri": "data:application/octet-stream;base64,AADIQgAAAAAAAAAAAADMQgAAAAAAAAAAAADMQgAAAEAAAAAAAADIQgAAAAAAAAAAAADMQgAAAEAAAAAAAADIQgAAAEAAAAAA" } ]
        })GLTF";
    }
    {
        ScopedModelOverride card(model.string().c_str());
        EngineHarness harness;
        SKIP_WITHOUT_FRAME_CAPTURE(harness);
        harness.useForwardRaster();
        harness.render_frames(SETTLE_FRAMES);
        ASSERT_FALSE(harness.renderer->hasDeviceLost());
        ASSERT_EQ(harness.scene->getModelCount(), 1U) << "the card did not load";

        EXPECT_NEAR(glm::distance(harness.camera->get_camera_position(), glm::vec3(101.0F, 1.0F, 10.0F)), 0.0F, 1e-3F);
        EXPECT_NEAR(glm::dot(harness.camera->get_camera_direction(), glm::vec3(0.0F, 0.0F, -1.0F)), 1.0F, 1e-5F);
        EXPECT_EQ(harness.feedback.loadedModelCredit, "Card by Somebody, CC0");
        // The renderer sets the lens from the Camera tab every frame, so the framing's must reach the tab.
        EXPECT_NEAR(harness.settings.scene.camera_fov, glm::degrees(0.4F), 1e-3F);
        EXPECT_NEAR(harness.camera->get_fov(), glm::degrees(0.4F), 1e-3F);

        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<uint8_t> rgba = harness.capture_frame(width, height);
        ASSERT_EQ(rgba.size(), static_cast<size_t>(width) * height * 4U);
        // The card fills the middle of the frame only when it is drawn from its own camera.
        const size_t centre = (static_cast<size_t>(height / 2U) * width + width / 2U) * 4U;
        const int red = rgba[centre];
        const int green = rgba[centre + 1U];
        const int blue = rgba[centre + 2U];
        EXPECT_GT(red, 2 * green + 40) << "centre " << red << ", " << green << ", " << blue;
        EXPECT_GT(red, 2 * blue + 40) << "centre " << red << ", " << green << ", " << blue;
        // The 2-unit card at 10 units spans 2 / (20 tan 0.2) = 0.493 of a 0.4 rad frame, 0.241 of a 45 degree one.
        size_t card_rows = 0;
        for (uint32_t y = 0; y < height; ++y) {
            const size_t pixel = (static_cast<size_t>(y) * width + width / 2U) * 4U;
            if (rgba[pixel] > 2 * rgba[pixel + 1U] + 40 && rgba[pixel] > 2 * rgba[pixel + 2U] + 40) { ++card_rows; }
        }
        const double share = static_cast<double>(card_rows) / static_cast<double>(height);
        EXPECT_NEAR(share, 2.0 / (20.0 * std::tan(0.2)), 0.02) << card_rows << " of " << height << " rows";
    }
    std::filesystem::remove_all(dir, ignored);
}
