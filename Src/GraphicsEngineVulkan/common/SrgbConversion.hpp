#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Kataglyphis {

// The decode an eR8G8B8A8Srgb sample applies.
inline auto srgbChannelToLinear(uint8_t encoded) -> float
{
    const float value = static_cast<float>(encoded) / 255.0F;
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

// The encode back to the nearest sRGB byte, clamped to [0, 1] first.
inline auto linearToSrgbByte(float linear) -> unsigned char
{
    const float v = std::clamp(linear, 0.0F, 1.0F);
    const float encoded = v <= 0.0031308F ? v * 12.92F : 1.055F * std::pow(v, 1.0F / 2.4F) - 0.055F;
    return static_cast<unsigned char>(std::lround(encoded * 255.0F));
}

}// namespace Kataglyphis
