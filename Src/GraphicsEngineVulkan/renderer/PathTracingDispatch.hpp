#pragma once

#include <cstdint>

// A plain header, not a module, so buildIntegritySuite.cpp can pin these to path_tracing.slang's [numthreads].
namespace Kataglyphis {

inline constexpr uint32_t kPathTracingWorkgroupSizeX = 8;
inline constexpr uint32_t kPathTracingWorkgroupSizeY = 8;

}// namespace Kataglyphis
