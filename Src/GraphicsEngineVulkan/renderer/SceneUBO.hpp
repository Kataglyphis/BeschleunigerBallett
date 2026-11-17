// Host mirror of the scene UBO; BuildIntegrity.SharedStructOffsetsMatchTheCompiledSpirv pins its layout.
#pragma once
#include "common/HostDeviceGlmAliases.hpp"
#include "common/host_device_shared_vars.hpp"

#include <glm/gtc/matrix_transform.hpp>
namespace Kataglyphis::VulkanRendererInternals {

struct DirectionalLightData
{
    vec4 direction;
    vec4 color;// w = radiance
};

struct SceneUBO
{
    // Directional light
    DirectionalLightData dirLight;

    uint pcfRadius;
    float cascadedShadowIntensity;
    uint numCascades;

    // Cube shadow maps drawn this frame (0: punctual lights unshadowed); also std140's pad before cascadeSplits.
    uint punctualShadowCount;

    // Cascaded shadow maps
    vec4 cascadeSplits;// up to 4 cascades
    static_assert(MAX_CASCADES <= 4, "cascadeSplits is a single vec4 - a fourth-plus cascade would write past its end");
    mat4 cascadeLightSpaceMatrices[MAX_CASCADES];

    // Camera: both .w are filler (SceneUboMarshal.hpp).
    vec4 view_dir;
    vec4 cam_pos;

    // Clouds: see docs/source/clouds.md § UBO packing.
    vec4 cloudLightMarch;
    vec4 cloudMeshScale;
    vec4 cloudMeshOffset;
    vec4 cloudParameters;
    // The sky's hemisphere means the clouds scatter; both .w are filler.
    vec4 cloudAmbientTop;
    vec4 cloudAmbientBottom;
};
}// namespace Kataglyphis::VulkanRendererInternals
