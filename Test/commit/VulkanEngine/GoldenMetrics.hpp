// CPU-only pixel metrics for the goldens, so goldenMetricsSuite.cpp can test them without a GPU.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

#include <glm/glm.hpp>

namespace Kataglyphis::Test::GoldenMetrics {

// A rectangular pixel region, half-open in both axes: [x0,x1) x [y0,y1).
struct Crop
{
    uint32_t x0;
    uint32_t x1;
    uint32_t y0;
    uint32_t y1;
};

// The right-hand region the goldens' thresholds were tuned on; EngineHarness hides the editor.
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

// Texture-detail proxy: fraction of `crop` whose right neighbour, also in `crop`, differs by more than `step` luma levels.
inline double detail_fraction(const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h, Crop crop, double step = 6.0)
{
    if (crop.x1 == 0U || crop.x1 <= crop.x0 + 1U) { return 0.0; }
    size_t detailed = 0;
    size_t total = 0;
    for (uint32_t y = crop.y0; y < crop.y1; ++y) {
        for (uint32_t x = crop.x0; x < crop.x1 - 1U; ++x) {
            const size_t base = static_cast<size_t>(y) * w + x;
            if (std::abs(luminance_of(rgba, base) - luminance_of(rgba, base + 1U)) > step) { ++detailed; }
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

// An axis-aligned box in world space, for the shadow goldens' analytic caster.
struct WorldBox
{
    glm::vec3 lo;
    glm::vec3 hi;
};

// Whether origin + t * dir meets `box` for some t in [1e-4, t_max]; the slab method, so an axis-parallel ray is fine.
inline bool ray_hits_box(glm::vec3 origin,
  glm::vec3 dir,
  const WorldBox &box,
  float t_max = std::numeric_limits<float>::infinity())
{
    float t_near = 1e-4F;
    float t_far = t_max;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(dir[axis]) < 1e-8F) {
            if (origin[axis] < box.lo[axis] || origin[axis] > box.hi[axis]) { return false; }
            continue;
        }
        const float t0 = (box.lo[axis] - origin[axis]) / dir[axis];
        const float t1 = (box.hi[axis] - origin[axis]) / dir[axis];
        t_near = std::max(t_near, std::min(t0, t1));
        t_far = std::min(t_far, std::max(t0, t1));
    }
    return t_near <= t_far;
}

// A finite horizontal receiver: y = height over [x0, x1] x [z0, z1].
struct GroundRect
{
    float height;
    float x0;
    float x1;
    float z0;
    float z1;
};

enum class GroundPixel : uint8_t { NotGround, Lit, Shadowed };

// Per pixel, what an exact directional-light shadow test says the ground there is; the camera ray stops at the caster.
struct GroundShadowPrediction
{
    std::vector<GroundPixel> pixels;
    std::vector<glm::vec3> world;
};

// `inv_view_proj` maps Vulkan NDC (y down, depth 0..1) to world space, as the renderer's camera matrices do.
inline GroundShadowPrediction predict_ground_shadow(const glm::mat4 &inv_view_proj,
  uint32_t w,
  uint32_t h,
  const GroundRect &ground,
  const WorldBox &caster,
  glm::vec3 light_travel)
{
    GroundShadowPrediction prediction{ std::vector<GroundPixel>(static_cast<size_t>(w) * h, GroundPixel::NotGround),
        std::vector<glm::vec3>(static_cast<size_t>(w) * h, glm::vec3(0.0F)) };
    const glm::vec3 to_light = -glm::normalize(light_travel);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const float ndc_x = (static_cast<float>(x) + 0.5F) / static_cast<float>(w) * 2.0F - 1.0F;
            const float ndc_y = (static_cast<float>(y) + 0.5F) / static_cast<float>(h) * 2.0F - 1.0F;
            const glm::vec4 near_h = inv_view_proj * glm::vec4(ndc_x, ndc_y, 0.0F, 1.0F);
            const glm::vec4 far_h = inv_view_proj * glm::vec4(ndc_x, ndc_y, 1.0F, 1.0F);
            const glm::vec3 origin = glm::vec3(near_h) / near_h.w;
            const glm::vec3 dir = glm::vec3(far_h) / far_h.w - origin;
            if (std::abs(dir.y) < 1e-8F) { continue; }
            const float t = (ground.height - origin.y) / dir.y;
            if (t <= 0.0F || t > 1.0F) { continue; }
            const glm::vec3 hit = origin + t * dir;
            if (hit.x < ground.x0 || hit.x > ground.x1 || hit.z < ground.z0 || hit.z > ground.z1) { continue; }
            // The caster between camera and ground hides the receiver; only a ray reaching the ground is compared.
            if (ray_hits_box(origin, hit - origin, caster, 0.9999F)) { continue; }
            const size_t pixel = static_cast<size_t>(y) * w + x;
            prediction.world[pixel] = hit;
            prediction.pixels[pixel] = ray_hits_box(hit, to_light, caster) ? GroundPixel::Shadowed : GroundPixel::Lit;
        }
    }
    return prediction;
}

// How a measured shadow mask agrees with the prediction, away from its edges; centroids in pixels.
struct ShadowAgreement
{
    size_t compared = 0;
    size_t agreeing = 0;
    size_t predicted_shadowed = 0;
    size_t measured_shadowed = 0;
    glm::dvec2 predicted_centroid{ 0.0 };
    glm::dvec2 measured_centroid{ 0.0 };

    [[nodiscard]] double agreement() const
    { return compared > 0U ? static_cast<double>(agreeing) / static_cast<double>(compared) : 0.0; }
};

// Compares only pixels whose whole (2 band + 1)^2 window is one predicted class: PCF and map texels blur the edge.
inline ShadowAgreement compare_shadow_masks(const std::vector<GroundPixel> &predicted,
  const std::vector<bool> &measured,
  uint32_t w,
  uint32_t h,
  uint32_t band)
{
    const size_t stride = static_cast<size_t>(w) + 1U;
    std::vector<uint32_t> lit_sum(stride * (h + 1U), 0U);
    std::vector<uint32_t> shadow_sum(stride * (h + 1U), 0U);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const GroundPixel kind = predicted[static_cast<size_t>(y) * w + x];
            const size_t at = (static_cast<size_t>(y) + 1U) * stride + x + 1U;
            const auto accumulate = [&](std::vector<uint32_t> &sum, bool hit) {
                sum[at] = (hit ? 1U : 0U) + sum[at - 1U] + sum[at - stride] - sum[at - stride - 1U];
            };
            accumulate(lit_sum, kind == GroundPixel::Lit);
            accumulate(shadow_sum, kind == GroundPixel::Shadowed);
        }
    }
    ShadowAgreement result;
    glm::dvec2 predicted_sum{ 0.0 };
    glm::dvec2 measured_sum{ 0.0 };
    for (uint32_t y = band; y + band < h; ++y) {
        for (uint32_t x = band; x + band < w; ++x) {
            const size_t x0 = x - band;
            const size_t y0 = y - band;
            const size_t x1 = static_cast<size_t>(x) + band + 1U;
            const size_t y1 = static_cast<size_t>(y) + band + 1U;
            const auto window = [&](const std::vector<uint32_t> &sum) {
                return sum[y1 * stride + x1] - sum[y0 * stride + x1] - sum[y1 * stride + x0] + sum[y0 * stride + x0];
            };
            const auto area = static_cast<uint32_t>((x1 - x0) * (y1 - y0));
            const bool all_lit = window(lit_sum) == area;
            const bool all_shadowed = window(shadow_sum) == area;
            if (!all_lit && !all_shadowed) { continue; }
            const bool shadowed = measured[static_cast<size_t>(y) * w + x];
            const glm::dvec2 at(static_cast<double>(x), static_cast<double>(y));
            ++result.compared;
            if (shadowed == all_shadowed) { ++result.agreeing; }
            if (all_shadowed) {
                ++result.predicted_shadowed;
                predicted_sum += at;
            }
            if (shadowed) {
                ++result.measured_shadowed;
                measured_sum += at;
            }
        }
    }
    if (result.predicted_shadowed > 0U) {
        result.predicted_centroid = predicted_sum / static_cast<double>(result.predicted_shadowed);
    }
    if (result.measured_shadowed > 0U) {
        result.measured_centroid = measured_sum / static_cast<double>(result.measured_shadowed);
    }
    return result;
}

// post.slang's display encoding of one linear channel, on a 0..255 scale: Narkowicz ACES, then the exact sRGB curve.
inline double post_display_level(double linear)
{
    const double x = std::max(linear, 0.0);
    const double aces = std::clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
    const double srgb = aces < 0.0031308 ? aces * 12.92 : 1.055 * std::pow(aces, 1.0 / 2.4) - 0.055;
    return srgb * 255.0;
}

}// namespace Kataglyphis::Test::GoldenMetrics
