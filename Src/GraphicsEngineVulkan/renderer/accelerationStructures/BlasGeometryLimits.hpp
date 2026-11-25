#pragma once

#include <cstdint>
#include <span>
#include <vulkan/vulkan.hpp>

#include "shared/scene/ObjMaterial.hpp"

namespace Kataglyphis {
struct BlasTriangleLimits
{
    uint32_t maxVertex;
    uint32_t primitiveCount;
};

// maxVertex is the highest index, not the count: robustBufferAccess is off, and an empty mesh must not wrap.
constexpr BlasTriangleLimits blasTriangleLimits(uint32_t vertexCount, uint32_t indexCount)
{ return { vertexCount == 0 ? 0 : vertexCount - 1, indexCount / 3 }; }

// One MASK material suffices: a mesh's geometry shares one BLAS entry and one flag.
constexpr bool blasGeometryNeedsAnyHit(std::span<const ObjMaterial> materials)
{
    for (const ObjMaterial &material : materials) {
        if (material.alphaMode == ALPHA_MASK) { return true; }
    }
    return false;
}

// eOpaque skips any-hit, so MASK geometry must stay non-opaque for raytrace.rahit.slang's alpha test.
constexpr vk::GeometryFlagsKHR blasGeometryFlags(bool needsAnyHit)
{ return needsAnyHit ? vk::GeometryFlagsKHR{} : vk::GeometryFlagsKHR{ vk::GeometryFlagBitsKHR::eOpaque }; }
}// namespace Kataglyphis
