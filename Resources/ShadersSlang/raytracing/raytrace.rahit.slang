import scene_types;
import rt_types;
import material_fetch;
import alpha_test;

// Also runs for the shadow ray, whose payload is a `bool`, so it must never write `payload`.

[shader("anyhit")]
void rahit_main(
    inout HitPayload payload,
    in BuiltInTriangleIntersectionAttributes attribs)
{
    // Per-mesh object description: same lookup as rchit_main.
    uint objIndex = InstanceID() + GeometryIndex();
    ObjectDescription obj = fetch_object_description(objIndex);

    if (ray_hit_masked_out(obj, PrimitiveIndex(), attribs.barycentrics))
    {
        IgnoreHit();
    }
}
