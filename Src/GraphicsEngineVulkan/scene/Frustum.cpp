module;
#include <algorithm>
#include <array>
#include <cmath>
#include <glm/glm.hpp>

module kataglyphis.vulkan.frustum;

namespace Kataglyphis {

namespace {
/// Keeps isVisible's epsilon scale-independent; a zero-length normal is left as is, since NaN would cull everything.
glm::vec4 normalizePlane(const glm::vec4 &plane)
{
    const float length = glm::length(glm::vec3(plane));
    if (length < 1e-8F) { return plane; }
    return plane / length;
}
}// namespace

FrustumPlanes extractFrustumPlanes(const glm::mat4 &m)
{
    // Gribb-Hartmann; GLM is column-major, so row k is m[0..3][k].
    const glm::vec4 row0{ m[0][0], m[1][0], m[2][0], m[3][0] };
    const glm::vec4 row1{ m[0][1], m[1][1], m[2][1], m[3][1] };
    const glm::vec4 row2{ m[0][2], m[1][2], m[2][2], m[3][2] };
    const glm::vec4 row3{ m[0][3], m[1][3], m[2][3], m[3][3] };

    FrustumPlanes planes{};
    planes[0] = normalizePlane(row3 + row0);// left
    planes[1] = normalizePlane(row3 - row0);// right
    planes[2] = normalizePlane(row3 + row1);// bottom
    planes[3] = normalizePlane(row3 - row1);// top
    // Near is row2 alone, not OpenGL's row3 + row2: GLM_FORCE_DEPTH_ZERO_TO_ONE makes it clip.z >= 0.
    planes[4] = normalizePlane(row2);// near
    planes[5] = normalizePlane(row3 - row2);// far
    return planes;
}

namespace {
/// `skipPlane` lets the shadow-caster variant drop the near plane.
bool visibleAgainstPlanes(const FrustumPlanes &planes, const AABB &box, int skipPlane)
{
    if (!box.isValid()) { return true; }

    for (int index = 0; index < static_cast<int>(planes.size()); ++index) {
        if (index == skipPlane) { continue; }
        const glm::vec4 &plane = planes[static_cast<size_t>(index)];
        const glm::vec3 normal{ plane };
        if (glm::dot(normal, normal) < 1e-16F) { continue; }

        const glm::vec3 positive{
            normal.x >= 0.0F ? box.max.x : box.min.x,
            normal.y >= 0.0F ? box.max.y : box.min.y,
            normal.z >= 0.0F ? box.max.z : box.min.z,
        };

        constexpr float kEpsilon = 1e-4F;
        if (glm::dot(normal, positive) + plane.w < -kEpsilon) { return false; }
    }
    return true;
}

/// Index of the near plane in the array extractFrustumPlanes builds.
constexpr int kNearPlaneIndex = 4;
}// namespace

bool isVisibleAsShadowCaster(const FrustumPlanes &planes, const AABB &box)
{
    return visibleAgainstPlanes(planes, box, kNearPlaneIndex);
}

bool isVisible(const FrustumPlanes &planes, const AABB &box)
{
    return visibleAgainstPlanes(planes, box, -1);
}

AABB transformAABB(const glm::mat4 &model, const AABB &box)
{
    if (!box.isValid()) { return box; }

    // Arvo's center/extent form; valid only for an affine `model`, never a projection.
    const glm::vec3 center = 0.5F * (box.min + box.max);
    const glm::vec3 extents = 0.5F * (box.max - box.min);

    const glm::vec3 newCenter{ model * glm::vec4(center, 1.0F) };
    glm::vec3 newExtents{ 0.0F };
    for (int row = 0; row < 3; ++row) {
        newExtents[row] = std::abs(model[0][row]) * extents.x + std::abs(model[1][row]) * extents.y
                         + std::abs(model[2][row]) * extents.z;
    }

    return AABB{ newCenter - newExtents, newCenter + newExtents };
}

}// namespace Kataglyphis
