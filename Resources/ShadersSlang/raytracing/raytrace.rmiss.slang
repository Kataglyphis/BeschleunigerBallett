import rt_types;

[[vk::push_constant]] PushConstantRaytracing pc_ray;

[shader("miss")]
void rmiss_main(inout HitPayload payload)
{
    payload.hit_value = pc_ray.clear_color.xyz * 0.4;
    payload.is_hit = 0.0;
}