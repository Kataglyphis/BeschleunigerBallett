#pragma once

#include <cstdint>

// Plain header, not a module, so buildIntegritySuite.cpp can pin these against the shaders' [numthreads].
namespace Kataglyphis {

inline constexpr uint32_t kNoiseVolumeExtent = 128;
inline constexpr uint32_t kNoiseWorkgroupSize = 8;
inline constexpr uint32_t kCloudWorkgroupSize = 16;

static_assert(kNoiseVolumeExtent % kNoiseWorkgroupSize == 0,
  "kNoiseVolumeExtent must tile evenly by kNoiseWorkgroupSize, or part of the noise volume goes unwritten");

// The minimums are load-bearing: both counts divide in clouds.slang, and zero steps put every sample at +-inf.
inline constexpr int kMinCloudMarchSteps = 4;
inline constexpr int kMaxCloudMarchSteps = 128;
inline constexpr int kMinCloudLightMarchSteps = 1;
inline constexpr int kMaxCloudLightMarchSteps = 128;

}// namespace Kataglyphis
