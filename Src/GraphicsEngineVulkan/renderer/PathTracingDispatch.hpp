#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <system_error>
#include <vector>

// A plain header, not a module, so the CPU suites and buildIntegrityTracingSuite.cpp read it without the engine.
namespace Kataglyphis {

inline constexpr uint32_t kPathTracingWorkgroupSizeX = 8;
inline constexpr uint32_t kPathTracingWorkgroupSizeY = 8;

// OxidANT's render/tracer/modes/dispatch.rs: a tile's side in pixels.
inline constexpr uint32_t kPathTracingTileSize = 256;
// Path segments one dispatch traces at most: milliseconds on a desktop GPU, far below Windows' 2 s TDR.
inline constexpr uint64_t kPathTracingDispatchSegments = uint64_t{ 1 } << 22U;

// KATAGLYPHIS_PT_DISPATCH_SEGMENTS may only lower Rust's budget, or it could lift a dispatch past the watchdog.
inline auto pathTracingDispatchBudget(std::string_view requested) -> uint64_t
{
    if (requested.empty()) { return kPathTracingDispatchSegments; }
    uint64_t segments = 0;
    const char *end = requested.data() + requested.size();
    const auto [parsed_to, error] = std::from_chars(requested.data(), end, segments);
    if (error != std::errc{} || parsed_to != end || segments == 0U) { return kPathTracingDispatchSegments; }
    return std::min(segments, kPathTracingDispatchSegments);
}

// One dispatch: a tile and the samples it adds to each of its pixels.
struct PathTracingDispatch
{
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    // The first of this dispatch's samples among the frame's.
    uint32_t first_sample = 0;
    uint32_t samples = 0;
    // Samples each of the tile's pixels held before this dispatch; 0 overwrites the pixel.
    uint32_t accumulated = 0;
};

// Samples one dispatch of a full tile takes, so it traces at most `budget` segments.
constexpr auto pathTracingSamplesPerDispatch(uint32_t samples,
  uint32_t segments_per_sample,
  uint64_t budget = kPathTracingDispatchSegments) -> uint32_t
{
    const uint64_t tile = uint64_t{ kPathTracingTileSize } * uint64_t{ kPathTracingTileSize }
                          * uint64_t{ std::max(segments_per_sample, 1U) };
    const uint64_t fit = std::clamp<uint64_t>(budget / tile, 1U, std::numeric_limits<uint32_t>::max());
    return std::min(static_cast<uint32_t>(fit), std::max(samples, 1U));
}

// The frame's dispatches: every pixel gets `samples` more after `accumulated`, chunk by chunk, each over every tile.
inline auto planPathTracingDispatches(uint32_t width,
  uint32_t height,
  uint32_t samples,
  uint32_t segments_per_sample,
  uint64_t accumulated,
  uint64_t budget = kPathTracingDispatchSegments) -> std::vector<PathTracingDispatch>
{
    width = std::max(width, 1U);
    height = std::max(height, 1U);
    samples = std::max(samples, 1U);
    const uint32_t chunk = pathTracingSamplesPerDispatch(samples, segments_per_sample, budget);
    const std::size_t tiles = static_cast<std::size_t>((width + kPathTracingTileSize - 1U) / kPathTracingTileSize)
                              * ((height + kPathTracingTileSize - 1U) / kPathTracingTileSize);
    std::vector<PathTracingDispatch> dispatches;
    dispatches.reserve(static_cast<std::size_t>((samples + chunk - 1U) / chunk) * tiles);
    constexpr uint64_t kMaxAccumulated = std::numeric_limits<uint32_t>::max();
    for (uint32_t first_sample = 0; first_sample < samples;) {
        const uint32_t count = std::min(chunk, samples - first_sample);
        const uint64_t before = std::min(accumulated, kMaxAccumulated - first_sample) + first_sample;
        for (uint32_t y = 0; y < height; y += kPathTracingTileSize) {
            for (uint32_t x = 0; x < width; x += kPathTracingTileSize) {
                dispatches.push_back(PathTracingDispatch{ .x = x,
                  .y = y,
                  .width = std::min(kPathTracingTileSize, width - x),
                  .height = std::min(kPathTracingTileSize, height - y),
                  .first_sample = first_sample,
                  .samples = count,
                  .accumulated = static_cast<uint32_t>(before) });
            }
        }
        first_sample += count;
    }
    return dispatches;
}

}// namespace Kataglyphis
