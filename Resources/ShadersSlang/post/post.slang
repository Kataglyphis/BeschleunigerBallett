import aces;
import fullscreen;

// C++/Vulkan post pass (tonemap + clouds); the Rust renderer composites in tonemap.slang instead.

// Combined image+sampler (Vulkan combined image sampler descriptor).
[vk::binding(0, 0)] Sampler2D noisyTxt;
[vk::binding(1, 0)] Sampler2D cloudTxt;

struct PushConstantPost
{
    uint clouds_enabled;
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

[shader("fragment")]
float4 fs_main(VsOut In) : SV_Target
{
    // Texel fetches: both inputs map 1:1 onto the swapchain, and filtering made coverage fractional at every edge.
    int3 texel = int3(int2(In.svPosition.xy), 0);
    // The sky is in the HDR target already, so every texel is opaque radiance.
    float3 color = noisyTxt.Load(texel).rgb;

    if (bool(pc_post.clouds_enabled))
    {
        // Premultiplied-over in linear HDR, over geometry and sky alike.
        float4 cloud = cloudTxt.Load(texel);
        color = cloud.rgb + color * (1.0 - cloud.a);
    }

    // ACES filmic tonemap (shared with Rust WebGPU renderer via aces.slang).
    float3 tonemapped = aces_tonemap(color);

    // The swapchain is UNORM (SwapchainChoices.hpp), so the shader must encode sRGB itself.
    return float4(linear_to_srgb(tonemapped), 1.0);
}
