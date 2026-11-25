// No ray-tracing types here (see rt_types.slang): their capabilities conflict with `discard` in fragment shaders.

// The limits and binding slots, which C++ includes too.
__exported import limits;
__exported import shared_constants;

// Vertex layout (matches Src/shared/scene/Vertex.hpp).
struct Vertex
{
    float3 position;
    float3 normal;
    float4 color;
    float2 texture_coords;
    // xyz = tangent direction, w = handedness (+-1); see Vertex.hpp.
    float4 tangent;
    // TEXCOORD_1; a copy of texture_coords when the asset has none.
    float2 texture_coords1;
};

// Material (matches Src/shared/scene/ObjMaterial.hpp, scalar layout).
struct ObjMaterial
{
    float3 diffuse;
    float3 emission;
    float shininess;
    float dissolve;
    int textureID;
    float alphaCutoff;
    float3 uv_transform_row0;
    float3 uv_transform_row1;
    float metallic;
    float roughness;
    int emissiveTextureID;
    int normalTextureID;
    float normalScale;
    int metallicRoughnessTextureID;
    float3 normal_uv_transform_row0;
    float3 normal_uv_transform_row1;
    float3 metallic_roughness_uv_transform_row0;
    float3 metallic_roughness_uv_transform_row1;
    float3 emissive_uv_transform_row0;
    float3 emissive_uv_transform_row1;
    int unlit;
    int alphaTextureID;
    int uv1SlotMask;
    int alphaBlend;
};

// Matches ObjectDescription.hpp; the uint64_t fields are buffer device addresses.
struct ObjectDescription
{
    uint64_t vertex_address;
    uint64_t index_address;
    uint64_t material_index_address;
    uint64_t material_address;
    uint64_t texture_offset;
};

// Buffer-reference types: Slang T* maps to SPIR-V PhysicalStorageBuffer.
struct Vertices { Vertex v[]; }
struct Indices { int3 i[]; }
struct MaterialIDs { int i[]; }
struct Materials { ObjMaterial m[]; }

// Clamped: a model past MAX_TEXTURE_COUNT samples a wrong slot rather than out of bounds.
int resolve_texture_slot(ObjectDescription obj, int textureID)
{
    return clamp(int(obj.texture_offset) + textureID, 0, MAX_TEXTURE_COUNT - 1);
}

int resolve_texture_slot(ObjectDescription obj, ObjMaterial material)
{
    return resolve_texture_slot(obj, material.textureID);
}


struct DirectionalLightData
{
    float4 direction; // the way the light travels; w = the sky's ambient strength
    float4 color; // w = radiance
};

struct GlobalUBO
{
    float4x4 projection;
    float4x4 view;
    float4x4 inv_projection;
    float4x4 inv_view;
};

// Clip to world for camera_ray, built from the two inverses GlobalUBO carries rather than a third matrix in its layout.
float4x4 clip_to_world(GlobalUBO ubo)
{
    return mul(ubo.inv_view, ubo.inv_projection);
}

struct SceneUBO
{
    DirectionalLightData dirLight;
    uint pcfRadius;
    float cascadedShadowIntensity;
    uint numCascades;
    uint punctualShadowCount;
    float4 cascadeSplits;
    float4x4 cascadeLightSpaceMatrices[CASCADE_COUNT];
    float4 view_dir;
    float4 cam_pos;
    float4 cloudLightMarch;
    float4 cloudMeshScale;
    float4 cloudMeshOffset;
    float4 cloudParameters;
    float4 cloudAmbientTop;
    float4 cloudAmbientBottom;
};