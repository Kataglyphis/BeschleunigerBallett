// CPU-only pixel metrics for the goldens, so goldenMetricsSuite.cpp can test them without a GPU.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

namespace Kataglyphis::Test::GoldenMetrics {

// A rectangular pixel region, half-open in both axes: [x0,x1) x [y0,y1).
struct Crop
{
    uint32_t x0;
    uint32_t x1;
    uint32_t y0;
    uint32_t y1;
};

// Excludes the ImGui overlay (the left ~72% plus thin top and bottom margins).
inline Crop panel_free_crop(uint32_t w, uint32_t h)
{ return Crop{ (w * 18U) / 25U, (w * 49U) / 50U, h / 20U, (h * 19U) / 20U }; }

// Isolates the right-hand wall a second model puts in frame, for the texture-detail golden.
inline Crop card_crop(uint32_t w, uint32_t h)
{ return Crop{ (w * 37U) / 50U, (w * 49U) / 50U, h / 20U, (h * 19U) / 20U }; }

// Rec. 709 luma of one RGBA8 pixel, on a 0..255 scale.
inline double luminance_of(const std::vector<uint8_t> &rgba, size_t pixel)
{
    const size_t base = pixel * 4U;
    return 0.2126 * static_cast<double>(rgba[base]) + 0.7152 * static_cast<double>(rgba[base + 1U])
           + 0.0722 * static_cast<double>(rgba[base + 2U]);
}

// Mean Rec. 709 luminance within `crop` (0..255), so goldens compare brightness without the overlay.
inline double mean_luminance_in_crop(const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h, Crop crop)
{
    double sum = 0.0;
    size_t count = 0;
    for (uint32_t y = crop.y0; y < crop.y1; ++y) {
        for (uint32_t x = crop.x0; x < crop.x1; ++x) {
            sum += luminance_of(rgba, static_cast<size_t>(y) * w + x);
            ++count;
        }
    }
    return count > 0U ? sum / static_cast<double>(count) : 0.0;
}

// Fraction of `crop` whose colour moves more than 5 levels on any channel between `a` and `b`.
inline double
  swung_fraction(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, uint32_t w, uint32_t h, Crop crop)
{
    size_t swung = 0;
    size_t total = 0;
    for (uint32_t y = crop.y0; y < crop.y1; ++y) {
        for (uint32_t x = crop.x0; x < crop.x1; ++x) {
            const size_t base = (static_cast<size_t>(y) * w + x) * 4U;
            for (size_t c = 0; c < 3U; ++c) {
                if (std::abs(static_cast<int>(a[base + c]) - static_cast<int>(b[base + c])) > 5) {
                    ++swung;
                    break;
                }
            }
            ++total;
        }
    }
    return total > 0U ? static_cast<double>(swung) / static_cast<double>(total) : 0.0;
}

// Texture-detail proxy: fraction of `crop` whose right neighbour, also in `crop`, differs by more than 6 luma levels.
inline double detail_fraction(const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h, Crop crop)
{
    if (crop.x1 == 0U || crop.x1 <= crop.x0 + 1U) { return 0.0; }
    size_t detailed = 0;
    size_t total = 0;
    for (uint32_t y = crop.y0; y < crop.y1; ++y) {
        for (uint32_t x = crop.x0; x < crop.x1 - 1U; ++x) {
            const size_t base = static_cast<size_t>(y) * w + x;
            if (std::abs(luminance_of(rgba, base) - luminance_of(rgba, base + 1U)) > 6.0) { ++detailed; }
            ++total;
        }
    }
    return total > 0U ? static_cast<double>(detailed) / static_cast<double>(total) : 0.0;
}

// Measured on the shadow rig: the slab is 42-44 levels off the sky, a jittered edge pixel 5-11 inside, a centred 0-3.
constexpr double SKY_SILHOUETTE_STEP = 20.0;
constexpr double SKY_SILHOUETTE_BLEND_MARGIN = 4.0;

// The first silhouette below the sky, per column: `columns` crossed it, `blended` had a pixel between sky and object.
struct SkySilhouette
{
    size_t columns = 0;
    size_t blended = 0;
    uint32_t first_row = std::numeric_limits<uint32_t>::max();
    uint32_t last_row = 0;
};

// Walks down from the crop's top, which must be sky; a step there is geometry, never a shadow or texture edge.
inline SkySilhouette sky_silhouette(const std::vector<uint8_t> &rgba, uint32_t w, Crop crop)
{
    SkySilhouette result;
    for (uint32_t x = crop.x0; x < crop.x1; ++x) {
        const auto lum = [&](uint32_t y) { return luminance_of(rgba, static_cast<size_t>(y) * w + x); };
        uint32_t row = crop.y0 + 1U;
        while (row + 2U < crop.y1 && std::abs(lum(row) - lum(row - 1U)) <= SKY_SILHOUETTE_BLEND_MARGIN) { ++row; }
        if (row + 2U >= crop.y1) { continue; }
        const double sky = lum(row - 1U);
        // Two rows down: past a blend that straddles two rows, and still on the object.
        const double object = lum(row + 2U);
        if (std::abs(object - sky) <= SKY_SILHOUETTE_STEP) { continue; }
        ++result.columns;
        result.first_row = std::min(result.first_row, row);
        result.last_row = std::max(result.last_row, row);
        const double edge = lum(row);
        const double lo = std::min(sky, object);
        const double hi = std::max(sky, object);
        if (edge > lo + SKY_SILHOUETTE_BLEND_MARGIN && edge < hi - SKY_SILHOUETTE_BLEND_MARGIN) { ++result.blended; }
    }
    return result;
}

}// namespace Kataglyphis::Test::GoldenMetrics
