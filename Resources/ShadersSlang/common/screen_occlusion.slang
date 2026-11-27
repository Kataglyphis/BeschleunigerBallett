import scene_types;
import surface_lighting;

// SSAO's half-resolution occlusion of the frame's raster depth (SsaoStage), which only the ambient light takes.
[vk::binding(SSAO_BINDING, 0)] Sampler2D ssaoOcclusion;

// SSAO at a fragment, filtered up to it: 1 while SSAO is off, and for a BLEND surface, whose texel lies behind it.
float scene_screen_occlusion(SceneUBO ubo, float4 svPosition, bool blended)
{
    float texel = ssaoOcclusion.SampleLevel(svPosition.xy * ubo.screenOcclusion.yz, 0.0).r;
    return screen_space_occlusion(texel, ubo.screenOcclusion.x, blended);
}
