// Grades a C++ and a Rust frame with GoldenMetrics.hpp's verdict: compare-renderer-pixels.sh's half that reads PNGs.

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "RendererFrameVerdict.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using Kataglyphis::Test::GoldenMetrics::frame_structure;
using Kataglyphis::Test::GoldenMetrics::FrameStructure;
namespace frames = Kataglyphis::Test::RendererFrames;

struct LoadedFrame
{
    std::optional<FrameStructure> structure;
    bool unreadable = false;
};

// A missing file is an absent frame; one stb_image cannot decode is a broken capture.
LoadedFrame load_frame(const std::string &path)
{
    std::error_code error;
    if (path.empty() || !std::filesystem::is_regular_file(path, error)) { return {}; }
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc *pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (pixels == nullptr || width <= 0 || height <= 0) {
        const char *reason = stbi_failure_reason();
        std::puts(
          std::format("FAIL {} is no image stb_image can read: {}.", path, reason != nullptr ? reason : "?").c_str());
        stbi_image_free(pixels);
        return { std::nullopt, true };
    }
    const std::span<const stbi_uc> bytes(pixels, static_cast<size_t>(width) * static_cast<size_t>(height) * 4U);
    const std::vector<uint8_t> rgba(bytes.begin(), bytes.end());
    stbi_image_free(pixels);
    return { frame_structure(rgba), false };
}

}// namespace

int main(int argc, char **argv)
{
    const std::span<char *> args(argv, static_cast<size_t>(argc));
    if (args.size() != 3U) {
        std::puts(
          "usage: compareRendererFrames <cpp-vulkan.png> <rust-webgpu.png> (a missing file is an absent frame)");
        return frames::kExitFailed;
    }
    const LoadedFrame cpp = load_frame(args[1]);
    const LoadedFrame rust = load_frame(args[2]);
    if (cpp.unreadable || rust.unreadable) {
        std::puts("=== PIXEL COMPARISON FAILED (exit code 1) ===");
        return frames::kExitFailed;
    }
    const frames::Verdict verdict = frames::grade_renderer_frames(cpp.structure, rust.structure);
    for (const std::string &line : verdict.lines) { std::puts(line.c_str()); }
    return verdict.exit_code;
}
