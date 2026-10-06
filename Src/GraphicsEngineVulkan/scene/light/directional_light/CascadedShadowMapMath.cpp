module;
#include <algorithm>
#include <array>
#include <cmath>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <span>
#include <vector>

#include "common/LightDirection.hpp"

module kataglyphis.vulkan.cascaded_shadow_map;

// Own TU of the module, so pure-math callers (perfSuite) never link Scene and its duplicate tinyobj implementation.

namespace Kataglyphis {

namespace {
    // Free so computeCascadeData() runs in tests without a Vulkan device.
    std::array<glm::vec4, 8> frustumCornersWorldSpace(const glm::mat4 &proj, const glm::mat4 &view)
    {
        const auto inv = glm::inverse(proj * view);

        std::array<glm::vec4, 8> frustumCorners{};
        std::size_t index = 0;
        for (unsigned int x = 0; x < 2; ++x) {
            for (unsigned int y = 0; y < 2; ++y) {
                for (unsigned int z = 0; z < 2; ++z) {
                    // NDC z runs 0..1, not -1..1: the engine builds with GLM_FORCE_DEPTH_ZERO_TO_ONE.
                    const glm::vec4 pt =
                      inv * glm::vec4((2.0F * x) - 1.0F, (2.0F * y) - 1.0F, static_cast<float>(z), 1.0F);
                    frustumCorners[index] = pt / pt.w;
                    ++index;
                }
            }
        }

        return frustumCorners;
    }
}// namespace

uint32_t clampCascadeCount(uint32_t requested, uint32_t maxCascades, uint32_t deviceViewLimit)
{
    uint32_t const clamped = std::min({ requested, maxCascades, deviceViewLimit });
    return std::max<uint32_t>(1U, clamped);
}

ShadowPushConstants makeShadowPush(const glm::mat4 &modelMatrix, uint32_t cascadeIndex)
{
    // Trivial but named, so a unit test pins that the caller's model matrix, not identity, reaches the GPU.
    return ShadowPushConstants{ modelMatrix, cascadeIndex };
}

ShadowSetBinding shadowSetBinding(bool hasSharedSet)
{
    // Without the shared set, firstSet = 1 keeps the light matrices at the layout's set 1, not 0.
    if (hasSharedSet) { return ShadowSetBinding{ 0, 2 }; }
    return ShadowSetBinding{ 1, 1 };
}

std::vector<CascadeData> computeCascadeData(uint32_t numCascades, const CascadeFitParams &params)
{
    // Allocating wrapper for callers off the frame path; the maths lives in computeCascadeDataInto.
    std::vector<CascadeData> cascadeData(numCascades);
    computeCascadeDataInto(cascadeData, numCascades, params);
    return cascadeData;
}

void computeCascadeDataInto(std::span<CascadeData> out, uint32_t numCascades, const CascadeFitParams &params)
{
    if (numCascades == 0U) { return; }
    // Refuse rather than clamp: an under-sized buffer would get a silently truncated cascade set.
    if (out.size() < numCascades) { return; }

    const glm::mat4 &cameraView = params.cameraView;
    const float cameraFov = params.cameraFov;
    const float aspect = params.aspect;
    const float nearPlane = params.nearPlane;
    const float farPlane = params.farPlane;
    const glm::vec3 &lightDir = params.lightDir;
    const float shadowDistance = params.shadowDistance;
    const float splitLambda = params.splitLambda;
    const uint32_t shadowMapResolution = params.shadowMapResolution;

    // Fit to shadowDistance, not the far plane, which wastes texels on empty space; 0 or less means the far plane.
    const float shadowFar = (shadowDistance > 0.0F) ? std::min(shadowDistance, farPlane) : farPlane;
    const float shadowNear = std::min(nearPlane, shadowFar * 0.5F);
    const float lambda = std::clamp(splitLambda, 0.0F, 1.0F);

    // Split as a function of i: no per-frame heap vector, and both endpoints stay exact.
    const auto cascadeSplit = [&](uint32_t i) -> float {
        if (i == 0U) { return shadowNear; }
        // Exact, not blended: a float-short last cascade leaves a band that renders unshadowed.
        if (i >= numCascades) { return shadowFar; }
        // Practical split scheme (Zhang et al.); lambda defaults to 0: higher values pay off only for close subjects.
        const float p = static_cast<float>(i) / static_cast<float>(numCascades);
        const float logSplit = shadowNear * std::pow(shadowFar / shadowNear, p);
        const float uniformSplit = shadowNear + ((shadowFar - shadowNear) * p);
        return (lambda * logSplit) + ((1.0F - lambda) * uniformSplit);
    };

    for (uint32_t i = 0; i < numCascades; i++) {
        const float splitNear = cascadeSplit(i);
        const float splitFar = cascadeSplit(i + 1);
        glm::mat4 const curr_cascade_proj = glm::perspective(glm::radians(cameraFov), aspect, splitNear, splitFar);

        std::array<glm::vec4, 8> frustumCornerWorldSpace = frustumCornersWorldSpace(curr_cascade_proj, cameraView);

        glm::vec3 center = glm::vec3(0, 0, 0);
        for (const auto &v : frustumCornerWorldSpace) { center += glm::vec3(v); }
        center /= frustumCornerWorldSpace.size();

        // The radius puts the light eye far enough back that the whole cascade sits in front of it.
        float radius = 0.0F;
        for (const auto &v : frustumCornerWorldSpace) { radius = std::max(radius, glm::length(glm::vec3(v) - center)); }

        const glm::vec3 light_direction = normalizedLightDirection(lightDir);
        const glm::vec3 up_axis =
          (std::abs(light_direction.y) > 0.99F) ? glm::vec3(0.0F, 0.0F, 1.0F) : glm::vec3(0.0F, 1.0F, 0.0F);

        if (shadowMapResolution > 0) {
            // Stable edges need a world-fixed basis, a radius-sized box, a texel-snapped center; near may go negative.
            glm::mat4 const light_basis = glm::lookAt(-light_direction, glm::vec3(0.0F), up_axis);

            // Pad one texel on the padded box, or the snap and projection grids drift; unsolvable at resolution <= 2.
            float half_extent = radius;
            float texel_world = 0.0F;
            if (shadowMapResolution > 2) {
                half_extent =
                  radius * static_cast<float>(shadowMapResolution) / static_cast<float>(shadowMapResolution - 2);
                texel_world = (2.0F * half_extent) / static_cast<float>(shadowMapResolution);
            }
            glm::vec3 center_ls = glm::vec3(light_basis * glm::vec4(center, 1.0F));
            if (texel_world > 0.0F) {
                center_ls.x = std::floor(center_ls.x / texel_world) * texel_world;
                center_ls.y = std::floor(center_ls.y / texel_world) * texel_world;
            }

            float snapMinZ = std::numeric_limits<float>::max();
            float snapMaxZ = std::numeric_limits<float>::lowest();
            for (const auto &m : frustumCornerWorldSpace) {
                glm::vec4 const v_light = light_basis * m;
                snapMinZ = std::min(snapMinZ, v_light.z);
                snapMaxZ = std::max(snapMaxZ, v_light.z);
            }
            constexpr float snapZPadding = 10.0F;
            float const snap_near = -snapMaxZ - snapZPadding;
            float snap_far = -snapMinZ + snapZPadding;
            if (snap_far <= snap_near) { snap_far = snap_near + 1.0F; }

            glm::mat4 const snap_projection = glm::ortho(center_ls.x - half_extent,
              center_ls.x + half_extent,
              center_ls.y - half_extent,
              center_ls.y + half_extent,
              snap_near,
              snap_far);

            out[i].viewProjMatrix = snap_projection * light_basis;
            out[i].splitDepth = splitFar;
            continue;
        }

        glm::mat4 const light_view_matrix =
          glm::lookAt(center - (light_direction * (radius * 2.0F + 10.0F)), center, up_axis);

        float minX = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float minY = std::numeric_limits<float>::max();
        float maxY = std::numeric_limits<float>::lowest();
        float minZ = std::numeric_limits<float>::max();
        float maxZ = std::numeric_limits<float>::lowest();

        for (const auto &m : frustumCornerWorldSpace) {
            glm::vec4 const v_light_view = light_view_matrix * m;
            minX = std::min(minX, v_light_view.x);
            maxX = std::max(maxX, v_light_view.x);
            minY = std::min(minY, v_light_view.y);
            maxY = std::max(maxY, v_light_view.y);
            minZ = std::min(minZ, v_light_view.z);
            maxZ = std::max(maxZ, v_light_view.z);
        }

        // Light view looks down -Z, so corner z is negative while glm::ortho takes positive near/far distances.
        constexpr float zPadding = 10.0F;// keep casters just outside the box
        float near_distance = std::max(0.01F, -maxZ - zPadding);
        float far_distance = (-minZ) + zPadding;
        if (far_distance <= near_distance) { far_distance = near_distance + 1.0F; }

        glm::mat4 const light_projection = glm::ortho(minX, maxX, minY, maxY, near_distance, far_distance);

        out[i].viewProjMatrix = light_projection * light_view_matrix;
        // Split depth is this cascade's far plane as a positive view-space distance.
        out[i].splitDepth = splitFar;
    }
}

}// namespace Kataglyphis
