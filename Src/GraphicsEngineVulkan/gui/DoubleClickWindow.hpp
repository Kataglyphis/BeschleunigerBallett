#pragma once

#include <algorithm>

namespace Kataglyphis::Frontend {

// Down, up and down reach ImGui a frame each: two frames and half again, at least 0.3 s, uncapped for QEMU's frames.
constexpr float doubleClickWindow(float previousDeltaTime, float deltaTime)
{ return std::max(1.5F * (previousDeltaTime + deltaTime), 0.3F); }

}// namespace Kataglyphis::Frontend
