import camera_ray;
import cloud_upsample;
import fullscreen;
import scene_types;
import volumetric_clouds;

// The march is OxidANT's shared volumetric_clouds.slang; this entry binds the C++ engine's resources to it.

[vk::binding(0, 0)] RWTexture2D<float4> outputImage;
[vk::binding(1, 0)] Sampler3D noiseTexture;
// NDC depth of what the main pass drew: a raster depth buffer, or the traced primary hits in RT/PT.
[vk::binding(2, 0)] Texture2D<float> sceneDepth;
// The half-resolution march, which only cloud_upsample_main reads.
[vk::binding(3, 0)] Texture2D<float4> halfClouds;

[vk::binding(0, 1)] ConstantBuffer<GlobalUBO> globalUBO;
[vk::binding(1, 1)] ConstantBuffer<SceneUBO> scene;

struct CombinedNoise : ICloudNoise
{
    // Explicit LOD: compute has no derivatives, so an implicit-LOD .Sample() fails SPIR-V validation.
    float4 sample_noise(float3 uvw)
    {
        return noiseTexture.SampleLevel(uvw, 0.0);
    }
};

// This engine's depth and half march along one pixel's view-space ray, for OxidANT's cloud_upsample.slang.
struct PixelTexels : ICloudUpsampleTexels
{
    float2 ndc;

    float scene_depth(int2 pixel)
    {
        return sceneDepth.Load(int3(pixel, 0));
    }

    float4 half_cloud(int2 texel)
    {
        return halfClouds.Load(int3(texel, 0));
    }

    // View space: the eye is the origin, and the view matrix keeps lengths, so this is the world distance too.
    float scene_distance(float depth)
    {
        return cloud_scene_distance(globalUBO.inv_projection, ndc, depth, float3(0.0));
    }
};

int2 scene_depth_size()
{
    uint width, height;
    sceneDepth.GetDimensions(width, height);
    return int2(int(width), int(height));
}

[shader("compute")]
[numthreads(16, 16, 1)]
void clouds_main(uint3 tid : SV_DispatchThreadID)
{
    int2 texelCoord = int2(tid.xy);
    uint width, height;
    outputImage.GetDimensions(width, height);
    int2 size = int2(int(width), int(height));

    if (texelCoord.x >= size.x || texelCoord.y >= size.y)
        return;

    // Through the texel centre, where the depth below was written and where post samples this texel.
    float2 ndc = fullscreen_uv_to_ndc((float2(texelCoord) + 0.5) / float2(size));
    // The march starts at the eye, so it takes the camera ray's direction alone.
    float3 rayDirection = camera_ray(clip_to_world(globalUBO), ndc).direction;

    CloudParams params;
    params.cloudMeshScale = scene.cloudMeshScale;
    params.cloudMeshOffset = scene.cloudMeshOffset;
    params.cloudParameters = scene.cloudParameters;
    params.cloudLightMarch = scene.cloudLightMarch;

    // An output smaller than the depth is the half-resolution march, which stops at its block's nearest surface.
    int2 depthSize = scene_depth_size();
    bool halfResolution = size.x < depthSize.x || size.y < depthSize.y;
    PixelTexels texels = { ndc };
    float depth = halfResolution ? half_texel_depth(texels, texelCoord, depthSize) : texels.scene_depth(texelCoord);
    float sceneDistance = texels.scene_distance(depth);

    // The sky this engine draws, which the host averaged per hemisphere, not OxidANT's analytic one.
    CloudAmbient ambient;
    ambient.top = scene.cloudAmbientTop.xyz;
    ambient.bottom = scene.cloudAmbientBottom.xyz;

    CombinedNoise noise;
    // dirLight.direction is where the light travels, so the vector toward it is the negation.
    float3 toLight = normalize(-scene.dirLight.direction.xyz);
    outputImage[tid.xy] = march_clouds(noise, scene.cam_pos.xyz, rayDirection, sceneDistance, cloud_unpack(params),
                                       toLight, scene.dirLight.color.xyz * scene.dirLight.color.w, ambient);
}

// The half-resolution march at full resolution, OxidANT's cloud_upsample.slang four-texel blend.
[shader("compute")]
[numthreads(16, 16, 1)]
void cloud_upsample_main(uint3 tid : SV_DispatchThreadID)
{
    int2 pixel = int2(tid.xy);
    uint width, height;
    outputImage.GetDimensions(width, height);
    if (pixel.x >= int(width) || pixel.y >= int(height))
        return;

    PixelTexels texels = { fullscreen_uv_to_ndc((float2(pixel) + 0.5) / float2(int2(int(width), int(height)))) };
    uint halfWidth, halfHeight;
    halfClouds.GetDimensions(halfWidth, halfHeight);
    outputImage[tid.xy] =
        cloud_upsample(texels, pixel, int2(int(halfWidth), int(halfHeight)), scene_depth_size());
}
