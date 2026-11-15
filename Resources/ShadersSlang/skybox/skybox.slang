// C++/Vulkan cubemap skybox.

import aces;
import skybox_radiance;

struct CameraUBO
{
    float4x4 projection;
    float4x4 view;
    // Precomputed on CPU (Slang has no inverse() for SPIR-V target).
    float4x4 inv_projection;
    float4x4 inv_view;
};
[vk::binding(0, 0)] ConstantBuffer<CameraUBO> camera;

[vk::binding(1, 1)] SamplerCube skyboxSampler;

struct PushConstantSkyBox
{
    uint skybox_enabled;
};
[[vk::push_constant]] PushConstantSkyBox pc_skybox;

struct VsOut
{
    float4 svPosition : SV_Position;
    float3 worldDir : TEXCOORD0;
};

[shader("vertex")]
VsOut vs_main(float3 in_position : POSITION,
              float3 in_normal : NORMAL,
              float4 in_color : COLOR,
              float2 in_tex_coord : TEXCOORD0)
{
    VsOut o;
    o.worldDir = skybox_view_direction(in_position.xy, camera.inv_projection, camera.inv_view);
    o.svPosition = float4(in_position.xy, 0.9999, 1.0);
    return o;
}

[shader("fragment")]
float4 fs_main(VsOut In) : SV_Target
{
    // Renders straight into the UNORM swapchain, so the shader encodes sRGB itself.
    return float4(linear_to_srgb(skybox_radiance(skyboxSampler, In.worldDir, pc_skybox.skybox_enabled != 0u)), 1.0);
}
