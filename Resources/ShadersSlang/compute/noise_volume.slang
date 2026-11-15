// This basename must not shadow OxidANT's shared noise module imported by the dual-target test.
import volumetric_clouds;

[vk::binding(0, 0)] RWTexture3D<float4> noiseVolume;

// NOISE_VOLUME_EXTENT (volumetric_clouds.slang) must equal CloudDispatch.hpp's kNoiseVolumeExtent.
[shader("compute")]
[numthreads(8, 8, 8)]
void noise_main(uint3 tid : SV_DispatchThreadID)
{
    noiseVolume[tid] = noise_volume_texel(tid);
}
