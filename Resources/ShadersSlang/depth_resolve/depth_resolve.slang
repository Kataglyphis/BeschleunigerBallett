import fullscreen;

// MSAA depth resolve keeping the nearest sample; the checked-in depth_resolve.wgsl is generated from this file.

[vk::binding(0, 0)] Texture2DMS<float> msaaDepth;

[shader("vertex")]
FullscreenVsOut vs_main(uint vid : SV_VertexID)
{
    // uv.y must be 0 at the top: fs_main reads at uv but writes at fragCoord, else the depth is mirrored.
    return fullscreen_vs(vid);
}

[shader("fragment")]
float fs_main(FullscreenVsOut In) : SV_Depth
{
    uint w, h, samples;
    msaaDepth.GetDimensions(w, h, samples);
    int2 coords = int2(In.uv * float2(float(w), float(h)));
    float minDepth = 1.0;
    for (uint i = 0u; i < samples; i++)
    {
        float d = msaaDepth.Load(int2(coords), i);
        minDepth = min(minDepth, d);
    }
    return minDepth;
}