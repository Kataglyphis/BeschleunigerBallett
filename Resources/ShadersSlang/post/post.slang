import aces;
import fullscreen;
import scene_types;
import skybox_radiance;

// C++/Vulkan post pass (tonemap + clouds); the Rust renderer composites in tonemap.slang instead.

// Combined image+sampler (Vulkan combined image sampler descriptor).
[vk::binding(0, 0)] Sampler2D noisyTxt;
[vk::binding(1, 0)] Sampler2D cloudTxt;
[vk::binding(2, 0)] SamplerCube skyboxTxt;
[vk::binding(globalUBO_BINDING, 1)] ConstantBuffer<GlobalUBO> globalUBO;

struct PushConstantPost
{
    uint clouds_enabled;
    uint skybox_enabled;
};
[[vk::push_constant]] PushConstantPost pc_post;

struct VsOut
{
    float4 svPosition : SV_Position;
    float2 uv : TEXCOORD0;
};

// SV_VulkanVertexID: SV_VertexID subtracts BaseVertex, which needs draw parameters a Raspberry Pi 5's v3dv lacks.
[shader("vertex")]
VsOut vs_main(uint vid : SV_VulkanVertexID)
{
    // Shared fullscreen-triangle vertex (no vertex buffer).
    FullscreenVsOut o = fullscreen_vs(vid);
    VsOut r;
    r.svPosition = o.svPosition;
    r.uv = o.uv;
    return r;
}

// Inverse of aces_tonemap on [0, 1]: the positive root of (a - c*y)*x^2 + (b - d*y)*x - e*y = 0.
float3 aces_tonemap_inverse(float3 display)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    float3 y = clamp(display, float3(0.0), float3(1.0));
    float3 qa = a - c * y;
    float3 qb = b - d * y;
    return (-qb + sqrt(qb * qb + 4.0 * qa * e * y)) / (2.0 * qa);
}

[shader("fragment")]
float4 fs_main(VsOut In) : SV_Target
{
    // Texel fetches: both inputs map 1:1 onto the swapchain, and filtering made coverage fractional at every edge.
    int3 texel = int3(int2(In.svPosition.xy), 0);
    float4 sampled = noisyTxt.Load(texel);
    float3 color = sampled.rgb;
    float  alpha = sampled.a;

    if (bool(pc_post.clouds_enabled))
    {
        // The sky pass is untonemapped, so the sky enters as the radiance whose tonemap reproduces its pixel.
        float3 direction =
            skybox_view_direction(fullscreen_uv_to_ndc(In.uv), globalUBO.inv_projection, globalUBO.inv_view);
        float3 sky = aces_tonemap_inverse(skybox_radiance(skyboxTxt, direction, pc_post.skybox_enabled != 0u));
        // Premultiplied-over in linear HDR, over geometry and sky alike; the result covers the sky pass.
        float4 cloud = cloudTxt.Load(texel);
        color = cloud.rgb + (color + sky * (1.0 - alpha)) * (1.0 - cloud.a);
        alpha = 1.0;
    }
    else if (alpha > 0.0 && alpha < 1.0)
    {
        // A BLEND surface over the sky leaves premultiplied colour; this pass's blend multiplies by alpha itself.
        color /= alpha;
    }

    // ACES filmic tonemap (shared with Rust WebGPU renderer via aces.slang).
    float3 tonemapped = aces_tonemap(color);

    // The swapchain is UNORM (SwapchainChoices.hpp), so the shader must encode sRGB itself.
    return float4(linear_to_srgb(tonemapped), alpha);
}
