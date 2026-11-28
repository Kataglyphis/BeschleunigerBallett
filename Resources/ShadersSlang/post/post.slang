import aces;
import exposure;
import fullscreen;

// C++/Vulkan post pass (clouds, exposure, bloom, tonemap); the Rust renderer composites in tonemap.slang instead.

// Combined image+sampler (Vulkan combined image sampler descriptor).
[vk::binding(0, 0)] Sampler2D noisyTxt;
[vk::binding(1, 0)] Sampler2D cloudTxt;
// [adapted EV, target EV] from histogram.slang's cs_reduce_exposure, the one exposure source, as in tonemap.slang.
[vk::binding(2, 0)] StructuredBuffer<float> exposureState;
// bloom.slang's half-resolution glow, already exposed; filtered up to the swapchain, as tonemap.slang samples it.
[vk::binding(3, 0)] Sampler2D bloomTxt;

struct PushConstantPost
{
    uint clouds_enabled;
    float bloom_strength;
    // 1 when a render scale sized the scene targets apart from the swapchain, which post then filters up.
    uint scene_scaled;
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

// Texel fetches at full scale, where filtering made coverage fractional at every edge; bilinear taps below it.
float4 post_input(Sampler2D source, VsOut In)
{
    if (bool(pc_post.scene_scaled))
        return source.SampleLevel(In.uv, 0.0);
    return source.Load(int3(int2(In.svPosition.xy), 0));
}

[shader("fragment")]
float4 fs_main(VsOut In) : SV_Target
{
    // The sky is in the HDR target already, so every texel is opaque radiance.
    float3 color = post_input(noisyTxt, In).rgb;

    if (bool(pc_post.clouds_enabled))
    {
        // Premultiplied-over in linear HDR, over geometry and sky alike.
        float4 cloud = post_input(cloudTxt, In);
        color = cloud.rgb + color * (1.0 - cloud.a);
    }

    // Exposure after the cloud composite, then the bloom, as tonemap.slang; ACES saturates long before 1e4.
    color *= exposure_scale(exposureState[0]);
    // A branch, not a multiply: a frame without bloom never wrote the target, and 0 times a NaN is a NaN.
    if (pc_post.bloom_strength > 0.0)
        color += bloomTxt.SampleLevel(In.uv, 0.0).rgb * pc_post.bloom_strength;
    color = min(color, float3(1e4));

    // ACES filmic tonemap (shared with Rust WebGPU renderer via aces.slang).
    float3 tonemapped = aces_tonemap(color);

    // The swapchain is UNORM (SwapchainChoices.hpp), so the shader must encode sRGB itself.
    return float4(linear_to_srgb(tonemapped), 1.0);
}
