import camera_ray;
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

// The nearest scene depth of the 2x2 pixels a half-resolution texel covers, as OxidANT's half_texel_depth.
float half_texel_depth(int2 texel)
{
    uint width, height;
    sceneDepth.GetDimensions(width, height);
    int2 last = int2(int(width), int(height)) - 1;
    float nearest = 1.0;
    for (int i = 0; i < 4; i++)
    {
        int2 pixel = min(texel * 2 + int2(i & 1, i >> 1), last);
        nearest = min(nearest, sceneDepth.Load(int3(pixel, 0)));
    }
    return nearest;
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
    uint depthWidth, depthHeight;
    sceneDepth.GetDimensions(depthWidth, depthHeight);
    bool halfResolution = width < depthWidth || height < depthHeight;
    float depth = halfResolution ? half_texel_depth(texelCoord) : sceneDepth.Load(int3(texelCoord, 0));
    // View space: the eye is the origin, and the view matrix keeps lengths, so this is the world distance too.
    float sceneDistance = cloud_scene_distance(globalUBO.inv_projection, ndc, depth, float3(0.0));

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

// The half-resolution march at full resolution: its four nearest texels, each weighted bilinearly and by depth.
[shader("compute")]
[numthreads(16, 16, 1)]
void cloud_upsample_main(uint3 tid : SV_DispatchThreadID)
{
    int2 pixel = int2(tid.xy);
    uint width, height;
    outputImage.GetDimensions(width, height);
    if (pixel.x >= int(width) || pixel.y >= int(height))
        return;

    float2 ndc = fullscreen_uv_to_ndc((float2(pixel) + 0.5) / float2(int2(int(width), int(height))));
    float pixelDistance =
        cloud_scene_distance(globalUBO.inv_projection, ndc, sceneDepth.Load(int3(pixel, 0)), float3(0.0));

    uint halfWidth, halfHeight;
    halfClouds.GetDimensions(halfWidth, halfHeight);
    int2 last = int2(int(halfWidth), int(halfHeight)) - 1;
    // This pixel's centre in half-resolution texels, whose centres sit at whole numbers.
    float2 position = (float2(pixel) + 0.5) * 0.5 - 0.5;
    int2 base = int2(floor(position));
    float2 f = position - float2(base);

    float4 weighted = float4(0.0);
    float4 bilinear = float4(0.0);
    float weightSum = 0.0;
    for (int i = 0; i < 4; i++)
    {
        int2 corner = int2(i & 1, i >> 1);
        int2 texel = clamp(base + corner, int2(0), last);
        float2 axis = lerp(1.0 - f, f, float2(corner));
        float w = axis.x * axis.y;
        float4 value = halfClouds.Load(int3(texel, 0));
        // The texel's depth along this pixel's ray: neighbours' rays differ by less than the depth edges matter.
        float texelDistance =
            cloud_scene_distance(globalUBO.inv_projection, ndc, half_texel_depth(texel), float3(0.0));
        float depthWeight = w * cloud_upsample_depth_weight(pixelDistance, texelDistance);
        weighted += value * depthWeight;
        weightSum += depthWeight;
        bilinear += value * w;
    }
    // No neighbour at this pixel's depth, a lone sliver: the plain bilinear blend beats a division by almost zero.
    outputImage[tid.xy] = weightSum > 1e-4 ? weighted / weightSum : bilinear;
}
