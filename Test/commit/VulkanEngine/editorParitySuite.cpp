// The Rust editor's command-line model, screenshot files, capture fences, drops and fly speed, without a device.

#include <gtest/gtest.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <glm/glm.hpp>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "ScopedTempDir.hpp"
#include "common/SceneUboMarshal.hpp"
#include <stb_image.h>
#include <vulkan/vulkan.hpp>

import kataglyphis.shared.util.utf8_path;
import kataglyphis.vulkan.camera;
import kataglyphis.vulkan.command_line;
import kataglyphis.vulkan.frame_capture;
import kataglyphis.vulkan.gui_scene_shared_vars;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.scene_config;
import kataglyphis.vulkan.screenshot_writer;

namespace {

using Kataglyphis::TestSupport::ScopedTempDir;

vk::Fence fakeFence(std::uint64_t id) { return vk::Fence(std::bit_cast<VkFence>(id)); }

bool writeFile(const std::filesystem::path &path, std::string_view text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

Kataglyphis::StartupModelArgument parse(std::vector<std::string> arguments)
{
    std::vector<char *> positional;
    positional.reserve(arguments.size());
    for (std::string &argument : arguments) { positional.push_back(argument.data()); }
    return Kataglyphis::startupModelArgument(std::span<char *const>(positional));
}

struct DecodedPng
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

std::optional<DecodedPng> readPng(const std::filesystem::path &path)
{
    DecodedPng png;
    int channels = 0;
    stbi_uc *pixels = stbi_load(path.string().c_str(), &png.width, &png.height, &channels, STBI_rgb_alpha);
    if (pixels == nullptr) { return std::nullopt; }
    png.rgba.assign(pixels, pixels + static_cast<std::size_t>(png.width) * static_cast<std::size_t>(png.height) * 4U);
    stbi_image_free(pixels);
    return png;
}

}// namespace

// Rust's viewer takes the model as its first argument; no argument leaves the startup chain to the environment.
TEST(CommandLineUnit, AnExistingModelIsTheStartupModel)
{
    const ScopedTempDir dir("command-line-model");
    ASSERT_FALSE(dir.path().empty());
    const std::filesystem::path model = dir.path() / "card.gltf";
    ASSERT_TRUE(writeFile(model, "{}"));

    const Kataglyphis::StartupModelArgument none = parse({});
    EXPECT_TRUE(none.model.empty());
    EXPECT_TRUE(none.note.empty());
    EXPECT_FALSE(none.tooMany);

    const Kataglyphis::StartupModelArgument given = parse({ model.string() });
    EXPECT_FALSE(given.tooMany);
    EXPECT_TRUE(given.note.empty()) << given.note;
    std::error_code error;
    EXPECT_TRUE(std::filesystem::equivalent(given.model, model, error)) << given.model;
    EXPECT_TRUE(std::filesystem::path(given.model).is_absolute()) << "outside Resources/ the model stays absolute";
}

// A file that is not there, or not a model, is reported and the start goes on without it, as Rust's chain does.
TEST(CommandLineUnit, AMissingOrUnusableFileIsReportedAndNotLoaded)
{
    const ScopedTempDir dir("command-line-missing");
    ASSERT_FALSE(dir.path().empty());
    const std::filesystem::path text = dir.path() / "notes.txt";
    ASSERT_TRUE(writeFile(text, "not a model"));

    const Kataglyphis::StartupModelArgument missing = parse({ (dir.path() / "gone.glb").string() });
    EXPECT_TRUE(missing.model.empty());
    EXPECT_FALSE(missing.tooMany);
    EXPECT_NE(missing.note.find("gone.glb does not exist"), std::string::npos) << missing.note;

    const Kataglyphis::StartupModelArgument folder = parse({ dir.path().string() });
    EXPECT_TRUE(folder.model.empty());
    EXPECT_NE(folder.note.find("is not a file"), std::string::npos) << folder.note;

    const Kataglyphis::StartupModelArgument not_a_model = parse({ text.string() });
    EXPECT_TRUE(not_a_model.model.empty());
    EXPECT_NE(not_a_model.note.find("is not a .gltf, .glb or .obj file"), std::string::npos) << not_a_model.note;

    const Kataglyphis::StartupModelArgument two = parse({ text.string(), text.string() });
    EXPECT_TRUE(two.tooMany);
    EXPECT_TRUE(two.model.empty());
}

// A file below Resources/Models loads by the path the library lists, so the library marks its row.
TEST(CommandLineUnit, AModelBelowResourcesModelsGoesByItsLibraryPath)
{
    const ScopedTempDir dir("library-path");
    ASSERT_FALSE(dir.path().empty());
    const std::filesystem::path models = dir.path() / "Resources" / "Models" / "Cards";
    std::error_code error;
    std::filesystem::create_directories(models, error);
    ASSERT_FALSE(error) << error.message();
    ASSERT_TRUE(writeFile(models / "card.glb", ""));
    ASSERT_TRUE(writeFile(dir.path() / "Resources" / "loose.obj", ""));

    const std::filesystem::path root = dir.path() / "Resources";
    const auto library = [](const std::filesystem::path &file, const std::filesystem::path &resources) {
        return sceneConfig::libraryPathFor(file, resources).value_or("");
    };
    EXPECT_EQ(std::filesystem::path(library(models / "card.glb", root)).generic_string(), "Models/Cards/card.glb");
    EXPECT_EQ(sceneConfig::modelIndexOf(
                std::array<std::string, 1>{ "Models/Cards/card.glb" }, library(models / "card.glb", root)),
      0);
    // The scan lists Models/ alone, and a file outside the root stays absolute.
    EXPECT_TRUE(std::filesystem::path(library(root / "loose.obj", root)).is_absolute());
    EXPECT_TRUE(std::filesystem::path(library(models / "card.glb", dir.path() / "Other")).is_absolute());
}

// A dropped name the Windows code page cannot hold is refused; path::string() threw there, ending the engine.
TEST(CommandLineUnit, ANameTheCodePageCannotHoldIsRefusedOrNamedExactly)
{
    const ScopedTempDir dir("narrow-path");
    ASSERT_FALSE(dir.path().empty());
    // No ANSI code page holds the emoji; Linux and a UTF-8 Windows name it.
    const std::filesystem::path model = dir.path() / std::filesystem::path(u8"card-\U0001F600.glb");
    ASSERT_TRUE(writeFile(model, ""));
    const std::optional<std::string> named = sceneConfig::libraryPathFor(model, dir.path() / "Resources");
    if (named.has_value()) {
        std::error_code error;
        EXPECT_TRUE(std::filesystem::equivalent(std::filesystem::path(*named), model, error))
          << *named << " names another file";
    }
#if !defined(_WIN32)
    EXPECT_TRUE(named.has_value()) << "a UTF-8 file system names every path";
#endif
    EXPECT_EQ(
      Kataglyphis::Shared::narrowPath(std::filesystem::path("Models/card.glb")).value_or(""), "Models/card.glb");
    EXPECT_EQ(Kataglyphis::Shared::utf8PathText(std::filesystem::path(u8"\u00fcber.glb")),
      "\xC3\xBC"
      "ber.glb");
}

// --help lists main's flags: abseil's default filter looked for a file named after the program and printed none.
TEST(CommandLineUnit, HelpListsTheFlagsOfMainCpp)
{
    EXPECT_TRUE(Kataglyphis::helpListsFlagsOf("/src/BeschleunigerBallett/Src/GraphicsEngineVulkan/Main.cpp"));
    EXPECT_TRUE(Kataglyphis::helpListsFlagsOf("C:\\src\\Src\\GraphicsEngineVulkan\\Main.cpp"));
    EXPECT_TRUE(Kataglyphis::helpListsFlagsOf("Main.cpp"));
    EXPECT_FALSE(Kataglyphis::helpListsFlagsOf("absl/flags/internal/usage.cc")) << "abseil's own flags stay out";
    EXPECT_FALSE(Kataglyphis::helpListsFlagsOf("Src/GraphicsEngineVulkan/NotMain.cpp"));
    EXPECT_NE(Kataglyphis::kUsage.find("BeschleunigerBallett [model]"), std::string_view::npos) << "the program's name";
}

// The Rust viewer's names: screenshot-000.png first, then the first number not taken.
TEST(ScreenshotWriterUnit, ShotsAreOpaquePngsNumberedFromZero)
{
    const ScopedTempDir dir("screenshot-names");
    ASSERT_FALSE(dir.path().empty());
    const std::filesystem::path shots = dir.path() / "screenshots";
    EXPECT_EQ(Kataglyphis::nextScreenshotPath(shots), shots / "screenshot-000.png");

    // Three by two, see-through: a swapchain's alpha must not reach the file.
    std::vector<std::uint8_t> rgba(3U * 2U * 4U);
    for (std::size_t i = 0; i < rgba.size(); ++i) { rgba[i] = static_cast<std::uint8_t>(i % 4U == 3U ? 0U : i * 9U); }

    const std::optional<std::filesystem::path> first = Kataglyphis::saveScreenshotPng(shots, rgba, 3, 2);
    ASSERT_TRUE(first.has_value()) << "the folder is made and the first shot written";
    EXPECT_EQ(*first, shots / "screenshot-000.png");
    const std::optional<std::filesystem::path> second = Kataglyphis::saveScreenshotPng(shots, rgba, 3, 2);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*second, shots / "screenshot-001.png");

    const std::optional<DecodedPng> png = readPng(*first);
    ASSERT_TRUE(png.has_value()) << "the shot is no PNG stb can read";
    EXPECT_EQ(png->width, 3);
    EXPECT_EQ(png->height, 2);
    ASSERT_EQ(png->rgba.size(), rgba.size());
    for (std::size_t i = 0; i < rgba.size(); ++i) {
        const int expected = (i % 4U == 3U) ? 255 : static_cast<int>(rgba[i]);
        EXPECT_EQ(static_cast<int>(png->rgba[i]), expected) << "byte " << i;
    }
}

// A failed step leaves no file behind and says so; the writer thread finishes before wait() returns.
TEST(ScreenshotWriterUnit, AFailedSaveWritesNothingAndTheWorkerJoins)
{
    const ScopedTempDir dir("screenshot-failures");
    ASSERT_FALSE(dir.path().empty());
    const std::filesystem::path blocker = dir.path() / "blocked";
    ASSERT_TRUE(writeFile(blocker, "a file where the folder should go"));
    const std::vector<std::uint8_t> pixel{ 1, 2, 3, 4 };
    EXPECT_FALSE(Kataglyphis::saveScreenshotPng(blocker, pixel, 1, 1).has_value());
    EXPECT_FALSE(Kataglyphis::saveScreenshotPng(dir.path(), pixel, 2, 1).has_value()) << "4 bytes are no 2x1 image";
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "screenshot-000.png"));

    Kataglyphis::ScreenshotWriter writer;
    writer.write(dir.path() / "async", pixel, 1, 1);
    writer.wait();
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "async" / "screenshot-000.png"));
}

// A second shot queues behind the first's encode instead of joining it on the frame thread; both land in order.
TEST(ScreenshotWriterUnit, AWriteNeverWaitsForTheShotBeforeIt)
{
    const ScopedTempDir dir("screenshot-queue");
    ASSERT_FALSE(dir.path().empty());
    // Noise, so the encode cannot shortcut: about a megabyte of RGBA8.
    constexpr std::uint32_t kSide = 512U;
    std::vector<std::uint8_t> noise(static_cast<std::size_t>(kSide) * kSide * 4U);
    std::uint32_t state = 0x9E3779B9U;
    for (std::uint8_t &byte : noise) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        byte = static_cast<std::uint8_t>(state);
    }
    const std::vector<std::uint8_t> pixel{ 9, 8, 7, 255 };

    Kataglyphis::ScreenshotWriter writer;
    const auto start = std::chrono::steady_clock::now();
    writer.write(dir.path(), noise, kSide, kSide);
    const auto first_queued = std::chrono::steady_clock::now();
    writer.write(dir.path(), pixel, 1, 1);
    const auto second_queued = std::chrono::steady_clock::now();
    writer.wait();
    const auto written = std::chrono::steady_clock::now();

    // Joining the first encode would hold the second write for most of the whole run.
    EXPECT_LT((second_queued - first_queued) * 4, written - start)
      << "the second write waited for the first shot's encode";
    const std::optional<DecodedPng> first = readPng(dir.path() / "screenshot-000.png");
    const std::optional<DecodedPng> second = readPng(dir.path() / "screenshot-001.png");
    ASSERT_TRUE(first.has_value() && second.has_value()) << "wait() returned before both shots were on disk";
    EXPECT_EQ(first->width, static_cast<int>(kSide)) << "the shots were written out of order";
    EXPECT_EQ(second->width, 1);
}

// The frame loop polls the copy's own frame, bound once; a later frame still running is no reason to keep waiting.
TEST(CaptureFenceUnit, TheCopysOwnFrameSaysItFinishedNotTheNewestOne)
{
    const vk::Fence copy_frame = fakeFence(1U);
    const vk::Fence next_frame = fakeFence(2U);
    const vk::Fence newest_frame = fakeFence(3U);
    std::vector<vk::Fence> signalled;
    const auto status = [&signalled](vk::Fence fence) {
        return std::find(signalled.begin(), signalled.end(), fence) != signalled.end() ? vk::Result::eSuccess
                                                                                       : vk::Result::eNotReady;
    };

    Kataglyphis::CaptureFence copy;
    EXPECT_FALSE(copy.finished(status)) << "nothing recorded, nothing finished";
    copy.recorded();
    copy.submitted(copy_frame);
    // A GPU-bound loop has submitted the frames after it, which have not finished when the next frame polls.
    copy.submitted(next_frame);
    copy.submitted(newest_frame);
    EXPECT_EQ(copy.waitFence(), copy_frame) << "a later submit rebound the copy to its own frame";
    EXPECT_FALSE(copy.finished(status));
    signalled = { copy_frame };
    EXPECT_TRUE(copy.finished(status)) << "the copy's frame finished, so a poll must collect it without a wait";
}

// Each frame waits on its slot's fence and resets it for its own submit; a copy that fence guarded has finished.
TEST(CaptureFenceUnit, TheSlotsWaitBeforeItsResetFinishesTheCopy)
{
    const vk::Fence slot_a = fakeFence(1U);
    const vk::Fence slot_b = fakeFence(2U);
    const auto never = [](vk::Fence) { return vk::Result::eNotReady; };

    Kataglyphis::CaptureFence copy;
    copy.recorded();
    copy.submitted(slot_a);
    copy.submitted(slot_b);
    copy.waited(slot_b);
    EXPECT_FALSE(copy.finished(never)) << "another slot's wait says nothing of the copy";
    copy.waited(slot_a);
    // Reset and submitted again, slot A's fence now answers for a newer frame.
    EXPECT_TRUE(copy.finished(never));
    EXPECT_FALSE(copy.waitFence()) << "take() must not wait on a fence a newer frame reuses";
    EXPECT_TRUE(copy.pending());
    EXPECT_TRUE(copy.wasSubmitted());
}

// A frame dropped before its submit leaves the staging buffer unwritten, so nothing may be saved from it.
TEST(CaptureFenceUnit, ACopyWhoseFrameNeverReachedTheQueueIsDropped)
{
    const vk::Fence slot = fakeFence(1U);
    const auto never = [](vk::Fence) { return vk::Result::eNotReady; };

    Kataglyphis::CaptureFence copy;
    copy.recorded();
    EXPECT_TRUE(copy.abandon());
    EXPECT_FALSE(copy.pending());
    EXPECT_FALSE(copy.abandon()) << "nothing was left to drop";
    copy.submitted(slot);
    EXPECT_FALSE(copy.pending()) << "a submit with nothing recorded bound a copy";

    copy.recorded();
    copy.drained();
    EXPECT_FALSE(copy.pending()) << "a drain kept a copy that never reached the queue";
    copy.recorded();
    copy.submitted(slot);
    EXPECT_FALSE(copy.abandon()) << "a submitted copy is no frame's to drop";
    copy.drained();
    EXPECT_TRUE(copy.finished(never)) << "a drain finished the submitted copy";
}

// A drop while a parse runs waits for it instead of joining it on the frame thread; the newest one wins.
TEST(ModelReplaceUnit, AReplaceDuringARunningParseWaitsWithoutJoiningIt)
{
    const ScopedTempDir dir("replace-queue");
    ASSERT_FALSE(dir.path().empty());
    // Many megabytes of triangles, so the parse outlasts the two calls below by far.
    const std::filesystem::path big = dir.path() / "big.obj";
    {
        std::ofstream out(big, std::ios::binary | std::ios::trunc);
        constexpr int kSide = 360;
        for (int y = 0; y < kSide; ++y) {
            for (int x = 0; x < kSide; ++x) { out << "v " << x << ' ' << y << " 0\n"; }
        }
        for (int y = 0; y + 1 < kSide; ++y) {
            for (int x = 0; x + 1 < kSide; ++x) {
                const int corner = y * kSide + x + 1;
                out << "f " << corner << ' ' << corner + 1 << ' ' << corner + kSide << '\n';
            }
        }
        ASSERT_TRUE(static_cast<bool>(out));
    }

    Kataglyphis::Scene scene;
    scene.beginModelReplaceAsync(big.string());
    ASSERT_TRUE(scene.isModelLoadPending());
    EXPECT_FALSE(scene.queuedModelReplace().has_value());
    scene.beginModelReplaceAsync("first-drop.glb");
    scene.beginModelReplaceAsync("second-drop.glb");
    ASSERT_TRUE(scene.queuedModelReplace().has_value()) << "the replace started at once, joining the running parse";
    EXPECT_EQ(*scene.queuedModelReplace(), "second-drop.glb") << "the newest drop must win";
    EXPECT_TRUE(scene.isModelLoadPending());
}

// The shot is the scene viewport in whole pixels: the full image with the editor hidden, never empty.
TEST(ScreenshotRegionUnit, TheViewportFractionsBecomeAWholePixelRect)
{
    using Kataglyphis::ViewportFraction;
    const Kataglyphis::ViewportPixels full = Kataglyphis::viewportPixels(1200, 768, ViewportFraction{});
    EXPECT_EQ(full.x, 0U);
    EXPECT_EQ(full.y, 0U);
    EXPECT_EQ(full.width, 1200U);
    EXPECT_EQ(full.height, 768U);

    const Kataglyphis::ViewportPixels docked =
      Kataglyphis::viewportPixels(1200, 768, ViewportFraction{ 0.2F, 0.05F, 0.72F, 0.8F });
    EXPECT_EQ(docked.x, 240U);
    EXPECT_EQ(docked.y, 38U);
    EXPECT_EQ(docked.width, 864U - 240U);
    EXPECT_EQ(docked.height, 614U - 38U);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const Kataglyphis::ViewportPixels collapsed =
      Kataglyphis::viewportPixels(1200, 768, ViewportFraction{ 1.5F, nan, -1.0F, nan });
    EXPECT_EQ(collapsed.x, 1199U);
    EXPECT_EQ(collapsed.width, 1U);
    EXPECT_EQ(collapsed.height, 1U);
    EXPECT_EQ(Kataglyphis::viewportPixels(0, 768, ViewportFraction{}).width, 0U);
}

// The Camera tab's Fly speed is OxidANT's: 10 by default, 0.001 to 1e6, and the camera travels that far a second.
TEST(FlySpeedUnit, TheSpeedSetsTheCamerasTravelPerSecond)
{
    const GUISceneSharedVars defaults;
    const Camera untouched;
    EXPECT_FLOAT_EQ(defaults.camera_fly_speed, kDefaultCameraFlySpeed);
    EXPECT_FLOAT_EQ(untouched.get_movement_speed(), defaults.camera_fly_speed) << "the tab and the camera disagree";

    EXPECT_FLOAT_EQ(Kataglyphis::cameraFlySpeed(42.0F), 42.0F);
    EXPECT_FLOAT_EQ(Kataglyphis::cameraFlySpeed(-3.0F), kMinCameraFlySpeed);
    EXPECT_FLOAT_EQ(Kataglyphis::cameraFlySpeed(5.0e6F), kMaxCameraFlySpeed);
    EXPECT_FLOAT_EQ(Kataglyphis::cameraFlySpeed(std::numeric_limits<float>::quiet_NaN()), kDefaultCameraFlySpeed);

    for (const float speed : { 0.5F, 40.0F }) {
        Camera camera;
        camera.set_movement_speed(Kataglyphis::cameraFlySpeed(speed));
        const glm::vec3 start = camera.get_camera_position();
        std::array<bool, GLFW_KEY_LAST + 1> keys{};
        keys[GLFW_KEY_W] = true;
        for (int step = 0; step < 4; ++step) { camera.key_control(keys, 0.25F); }
        EXPECT_NEAR(glm::length(camera.get_camera_position() - start), speed, speed * 1e-4F)
          << "a second of W at " << speed << " units per second";
    }
}
