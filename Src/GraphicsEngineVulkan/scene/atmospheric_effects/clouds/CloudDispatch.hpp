#pragma once

#include <cstdint>

// Plain header, not a module, so buildIntegritySuite.cpp can pin these against the shaders' [numthreads].
namespace Kataglyphis {

inline constexpr uint32_t kNoiseVolumeExtent = 128;
inline constexpr uint32_t kNoiseWorkgroupSize = 8;
inline constexpr uint32_t kCloudWorkgroupSize = 16;

// The half-resolution march covers every full-size pixel, so an odd extent rounds up.
inline constexpr uint32_t cloudHalfExtent(uint32_t full) { return (full + 1U) / 2U; }
static_assert(cloudHalfExtent(1201) * 2U >= 1201U && cloudHalfExtent(1200) == 600U && cloudHalfExtent(1) == 1U,
  "every full-size pixel needs a half-resolution texel");

// Descriptor sets of the clouds' one layout: the two marches differ only in the image they write.
inline constexpr uint32_t kCloudFullMarchSet = 0;
inline constexpr uint32_t kCloudHalfMarchSet = 1;
inline constexpr uint32_t kCloudUpsampleSet = 2;
inline constexpr uint32_t kCloudDescriptorSetCount = 3;

static_assert(kNoiseVolumeExtent % kNoiseWorkgroupSize == 0,
  "kNoiseVolumeExtent must tile evenly by kNoiseWorkgroupSize, or part of the noise volume goes unwritten");

// The minimums are load-bearing: both counts divide in the cloud march, and zero steps put every sample at +-inf.
inline constexpr int kMinCloudMarchSteps = 4;
inline constexpr int kMaxCloudMarchSteps = 128;
inline constexpr int kMinCloudLightMarchSteps = 1;
inline constexpr int kMaxCloudLightMarchSteps = 128;

}// namespace Kataglyphis
