// C++/Vulkan cubemap skybox.

import aces;

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
    float4 clipPos = float4(in_position.x, in_position.y, 1.0, 1.0);
    float4 viewPos = mul(camera.inv_projection, clipPos);
    float3 viewDir = float3(viewPos.xy, -1.0);
    o.worldDir = mul((float3x3)camera.inv_view, viewDir);
    o.svPosition = float4(in_position.xy, 0.9999, 1.0);
    return o;
}

[shader("fragment")]
float4 fs_main(VsOut In) : SV_Target
{
    // Renders straight into the UNORM swapchain, so both branches encode sRGB themselves.
    if (pc_skybox.skybox_enabled != 0u)
    {
        return float4(linear_to_srgb(skyboxSampler.Sample(normalize(In.worldDir)).xyz), 1.0);
    }
    return float4(linear_to_srgb(float3(0.0, 0.0, 0.5)), 1.0);
}