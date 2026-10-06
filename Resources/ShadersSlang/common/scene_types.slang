// No ray-tracing types here (see rt_types.slang): their capabilities conflict with `discard` in fragment shaders.

static const int MAX_TEXTURE_COUNT = 128;

// ----- MAIN RENDER DESCRIPTOR SET (shared between rasterizer and raytracer)
static const int globalUBO_BINDING = 0;
static const int sceneUBO_BINDING = 1;
static const int OBJECT_DESCRIPTION_BINDING = 2;
static const int TEXTURES_BINDING = 3;
static const int SAMPLER_BINDING = 4;
static const int SHADOW_MAP_BINDING = 5;

// ---- RAYTRACING BINDING (only used by RT/path-tracing shaders)
static const int TLAS_BINDING = 0;
static const int OUT_IMAGE_BINDING = 1;
static const int ACCUMULATION_IMAGE_BINDING = 2;

// G-buffer input attachments (set 1): lightingInputRefs positions, not render pass indices (those are +1).
static const int GBUFFER_NORMAL_BINDING = 0;
static const int GBUFFER_ALBEDO_BINDING = 1;
static const int GBUFFER_MATERIAL_BINDING = 2;
static const int GBUFFER_DEPTH_BINDING = 3;

// Vertex layout (matches Src/shared/scene/Vertex.hpp).
struct Vertex
{
    float3 position;
    float3 normal;
    float4 color;
    float2 texture_coords;
    // xyz = tangent direction, w = handedness (+-1); see Vertex.hpp.
    float4 tangent;
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

static const int MAX_CASCADES = 3;
static const int MAX_PCF_RADIUS = 5;

struct DirectionalLightData
{
    float4 direction;
    float4 color; // w = radiance
};

struct GlobalUBO
{
    float4x4 projection;
    float4x4 view;
    float4x4 inv_projection;
    float4x4 inv_view;
};

struct SceneUBO
{
    DirectionalLightData dirLight;
    uint pcfRadius;
    float cascadedShadowIntensity;
    uint numCascades;
    float4 cascadeSplits;
    float4x4 cascadeLightSpaceMatrices[MAX_CASCADES];
    float4 view_dir;
    float4 cam_pos;
    float4 cloudLightMarch;
    float4 cloudMeshScale;
    float4 cloudMeshOffset;
    float4 cloudParameters;
};