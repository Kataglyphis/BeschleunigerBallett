import scene_types;
import volumetric_clouds;

// The march is OxidANT's shared volumetric_clouds.slang; this entry binds the C++ engine's resources to it.

[vk::binding(0, 0)] RWTexture2D<float4> outputImage;
[vk::binding(1, 0)] Sampler3D noiseTexture;
// NDC depth of what the main pass drew: a raster depth buffer, or the traced primary hits in RT/PT.
[vk::binding(2, 0)] Texture2D<float> sceneDepth;

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
    float2 uv = (float2(texelCoord) + 0.5) / float2(size);
    float2 ndc = uv * 2.0 - 1.0;

    float4 clip = float4(ndc, 1.0, 1.0);
    float4 eyeCoords = mul(globalUBO.inv_projection, clip);
    eyeCoords = float4(eyeCoords.xy, -1.0, 0.0);
    float3 rayDirection = normalize(mul(globalUBO.inv_view, eyeCoords).xyz);

    CloudParams params;
    params.cloudMeshScale = scene.cloudMeshScale;
    params.cloudMeshOffset = scene.cloudMeshOffset;
    params.cloudParameters = scene.cloudParameters;
    params.cloudLightMarch = scene.cloudLightMarch;

    // View space: the eye is the origin, and the view matrix keeps lengths, so this is the world distance too.
    float depth = sceneDepth.Load(int3(texelCoord, 0));
    float sceneDistance = cloud_scene_distance(globalUBO.inv_projection, ndc, depth, float3(0.0));

    CombinedNoise noise;
    // dirLight.direction is where the light travels, so the vector toward it is the negation.
    float3 toLight = normalize(-scene.dirLight.direction.xyz);
    outputImage[tid.xy] = march_clouds(noise, scene.cam_pos.xyz, rayDirection, sceneDistance, cloud_unpack(params),
                                       toLight, scene.dirLight.color.xyz * scene.dirLight.color.w);
}
