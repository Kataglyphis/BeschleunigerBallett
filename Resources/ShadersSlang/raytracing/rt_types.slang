// Ray-tracing-only types, kept out of scene_types.slang so raster shaders never import them.

// Ray payload carried between rgen -> rchit/rmiss.
struct HitPayload
{
    float3 hit_value;
    float is_hit;
};

// Push constants for the ray tracing pass.
struct PushConstantRaytracing
{
    float4 clear_color;
};