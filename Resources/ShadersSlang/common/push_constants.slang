// Hand-mirrored by PushConstantRasterizer.hpp; the vertex stage derives the normal transform from model.
struct PushConstantRasterizer
{
    float4x4 model;
    uint objectIndex;
};
