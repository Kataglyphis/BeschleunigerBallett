module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "spdlog/spdlog.h"
#define TINYOBJLOADER_IMPLEMENTATION
#define TINYOBJLOADER_DISABLE_FAST_FLOAT
#include <glm/ext/vector_float2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <algorithm>
#include <iterator>
#include <iostream>
#include <tiny_obj_loader.h>
#include <unordered_map>

module kataglyphis.vulkan.obj_loader;

import kataglyphis.vulkan.vertex;
import kataglyphis.vulkan.device;
import kataglyphis.vulkan.obj_material;
import kataglyphis.vulkan.model;
import kataglyphis.vulkan.texture;
import kataglyphis.shared.util.file_reader;
import kataglyphis.vulkan.mesh_range;
import kataglyphis.vulkan.model_assembly;

using namespace Kataglyphis;

ObjLoader::ObjLoader(const std::shared_ptr<VulkanDevice> &device, vk::CommandPool command_pool)
  : device(device), command_pool(command_pool)
{}

bool ObjLoader::parseCpu(const std::string &modelFile)
{
    // clear prior state if called multiple times on the same instance
    textures.clear();
    textureSrgb.clear();
    materials.clear();
    vertices.clear();
    indices.clear();
    materialIndex.clear();
    meshRanges.clear();

    tinyobj::ObjReaderConfig const reader_config;
    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(modelFile, reader_config)) {
        // Fail softly: the GUI model picker can hand this arbitrary files.
        if (!reader.Error().empty()) { spdlog::error("TinyObjReader: {}", reader.Error()); }
        return false;
    }
    if (!reader.Warning().empty()) { spdlog::warn("TinyObjReader: {}", reader.Warning()); }

    loadTexturesAndMaterials(reader, modelFile);
    loadVertices(reader);
    return true;
}

auto ObjLoader::loadModel(const std::string &modelFile) -> std::shared_ptr<Model>
{
    using clock = std::chrono::steady_clock;
    const auto load_started = clock::now();

    if (!parseCpu(modelFile)) { return nullptr; }

    const auto parse_done = clock::now();
    std::shared_ptr<Model> new_model = uploadParsed();
    const auto upload_done = clock::now();

    const auto ms = [](auto from, auto to) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
    };
    // Split where the device work starts: the parse can move to a worker, the upload cannot.
    spdlog::info(
      "Model load: CPU parse {} ms (threadable), GPU textures+upload {} ms (must stay on this thread), "
      "total {} ms ({} verts, {} indices)",
      ms(load_started, parse_done),
      ms(parse_done, upload_done),
      ms(load_started, upload_done),
      vertices.size(),
      indices.size());

    return new_model;
}

auto ObjLoader::uploadParsed() -> std::shared_ptr<Model>
{
    // Must run on the device thread; parseCpu can run anywhere.
    if (!uploadPreconditionsMet(device, vertices.size(), "ObjLoader")) { return nullptr; }

    std::shared_ptr<Model> new_model = std::make_shared<Model>(device);

    const std::vector<std::string> &textureNames = textures;

    // now that we have the names lets create the vulkan side of textures
    for (size_t i = 0; i < textureNames.size(); i++) {
        if (!textureNames[i].empty()) {
            Texture texture;
            const bool created =
              texture.createFromFile(device, command_pool, textureNames[i], textureSrgb[i] != 0);
            addTextureOrDefault(*new_model, device, command_pool, created, std::move(texture));
        }
    }

    ensureAtLeastOneTexture(*new_model, device, command_pool);

    // One mesh per OBJ shape; each shares the full materials array, since materialIndex keeps the original indices.
    addMeshesForRanges(
      *new_model, device, command_pool, vertices, indices, materialIndex, this->materials, meshRanges);
    return new_model;
}

std::string Kataglyphis::resolveObjTexturePath(const std::string &baseDir, const std::string &mapKd)
{
    // See docs/model-loading.md § `map_Kd` texture path resolution.
    std::string normalized_map_kd = mapKd;
    std::replace(normalized_map_kd.begin(), normalized_map_kd.end(), '\\', '/');

    // An empty baseDir must not become a leading "/" that resolves against the filesystem root.
    const std::string base = baseDir.empty() ? "." : baseDir;

    const std::string beside_mtl = base + "/" + normalized_map_kd;
    const std::string under_textures = base + "/textures/" + normalized_map_kd;

    std::error_code beside_mtl_ec;
    if (std::filesystem::exists(beside_mtl, beside_mtl_ec) && !beside_mtl_ec) { return beside_mtl; }

    std::error_code under_textures_ec;
    if (std::filesystem::exists(under_textures, under_textures_ec) && !under_textures_ec) {
        return under_textures;
    }

    // Warn loudly: otherwise a wrong path silently renders the default white texture.
    spdlog::warn(
      "texture '{}' not found beside the .mtl ('{}') or under textures/ ('{}'); the model will "
      "render with the default texture",
      mapKd,
      beside_mtl,
      under_textures);
    return beside_mtl;
}

void ObjLoader::loadTexturesAndMaterials(const tinyobj::ObjReader &reader, const std::string &modelFile)
{
    const auto &tol_materials = reader.GetMaterials();
    textures.reserve(tol_materials.size());

    int texture_id = 0;
    const std::string base_dir = Kataglyphis::Shared::getBaseDir(modelFile);

    // Keyed on (resolved path, srgb): one file used as both sRGB and linear needs two slots.
    std::map<std::pair<std::string, bool>, int> pathSlot;

    // Always appends to textures/textureSrgb: uploadParsed walks them 1:1 with materials.
    const auto resolveSlot = [&](const std::string &texname, bool srgb) -> int {
        const std::string resolved = resolveObjTexturePath(base_dir, texname);
        const auto key = std::make_pair(resolved, srgb);
        const auto existing = pathSlot.find(key);
        if (existing != pathSlot.end()) {
            // An empty entry keeps the 1:1 walk while skipping the duplicate upload.
            textures.emplace_back("");
            textureSrgb.push_back(srgb ? 1 : 0);
            return existing->second;
        }
        pathSlot[key] = texture_id;
        textures.push_back(resolved);
        textureSrgb.push_back(srgb ? 1 : 0);
        return texture_id++;
    };

    // we now iterate over all materials to get diffuse and normal textures
    for (const auto &tol_material : tol_materials) {
        const tinyobj::material_t *mp = &tol_material;
        ObjMaterial material{};
        material.diffuse = glm::vec3(mp->diffuse[0], mp->diffuse[1], mp->diffuse[2]);
        material.emission = glm::vec3(mp->emission[0], mp->emission[1], mp->emission[2]);
        material.dissolve = mp->dissolve;
        material.shininess = mp->shininess;
        material.metallic = mp->metallic;
        // tinyobjloader reports an absent Pr as 0.0, so 0.0 keeps the -1 sentinel (roughness from shininess).
        if (mp->roughness > 0.0F) { material.roughness = mp->roughness; }

        if (!mp->diffuse_texname.empty()) {
            material.textureID = resolveSlot(mp->diffuse_texname, true);
        } else {
            // -1, not 0: slot 0 would sample a texture instead of material.diffuse.
            material.textureID = -1;
            textures.emplace_back("");
            textureSrgb.push_back(1);
        }

        // norm wins over map_Bump, and -bm comes from whichever directive won (twin: obj_to_gltf.rs).
        if (!mp->normal_texname.empty()) {
            material.normalTextureID = resolveSlot(mp->normal_texname, false);
            material.normalScale = mp->normal_texopt.bump_multiplier;
        } else if (!mp->bump_texname.empty()) {
            spdlog::debug(
              "ObjLoader: material '{}' has no 'norm' directive; using 'map_Bump' ('{}') as the normal map",
              mp->name,
              mp->bump_texname);
            material.normalTextureID = resolveSlot(mp->bump_texname, false);
            material.normalScale = mp->bump_texopt.bump_multiplier;
        } else {
            // No push without a directive, unlike the diffuse slot, so textures stays as long as materials.
            material.normalTextureID = -1;
        }

        // map_Ke is authored colour, so sRGB.
        if (!mp->emissive_texname.empty()) {
            material.emissiveTextureID = resolveSlot(mp->emissive_texname, true);
        } else {
            material.emissiveTextureID = -1;
        }

        // map_d is a linear opacity mask, treated as glTF MASK at 0.5: there is no sorted transparent pass.
        if (!mp->alpha_texname.empty()) {
            material.alphaTextureID = resolveSlot(mp->alpha_texname, false);
            material.alphaCutoff = 0.5F;
        } else {
            material.alphaTextureID = -1;
        }

        materials.push_back(material);
    }

    // No .mtl: one default material.
    if (tol_materials.empty()) { materials.emplace_back(); }
}

void ObjLoader::loadVertices(const tinyobj::ObjReader &reader)
{
    const auto &attrib = reader.GetAttrib();
    const auto &shapes = reader.GetShapes();

    // Only indices is reserved in full; vertices are mostly shared, so a face-vertex reserve would overshoot.
    size_t total_face_vertices = 0;
    for (const auto &shape : shapes) { total_face_vertices += shape.mesh.indices.size(); }
    indices.reserve(total_face_vertices);

    // Dedup per shape, so each shape's vertices stay one contiguous block uploadParsed can slice.
    for (const auto &shape : shapes) {
        std::unordered_map<Vertex, uint32_t> vertices_map{};
        vertices_map.reserve(shape.mesh.indices.size());

        const size_t shape_vertex_base = vertices.size();
        const size_t shape_index_start = indices.size();
        const size_t shape_tri_start = materialIndex.size();

        // prepare for enlargement
        vertices.reserve(shape.mesh.indices.size() + vertices.size());
        indices.reserve(shape.mesh.indices.size() + indices.size());

        // Loop over faces(polygon)
        size_t index_offset = 0;
        for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); f++) {
            auto const fv = static_cast<size_t>(shape.mesh.num_face_vertices[f]);

            // A bad index drops the whole face: dropping one corner would desync indices from materialIndex.
            bool face_valid = true;
            for (size_t v = 0; v < fv; v++) {
                tinyobj::index_t const idx = shape.mesh.indices[index_offset + v];
                const auto vertex_index = static_cast<size_t>(idx.vertex_index);
                if (idx.vertex_index < 0 || (3 * vertex_index) + 2 >= attrib.vertices.size()) {
                    face_valid = false;
                    break;
                }
            }

            if (face_valid) {
                // Loop over vertices in the face.
                for (size_t v = 0; v < fv; v++) {
                    // access to vertex
                    tinyobj::index_t const idx = shape.mesh.indices[index_offset + v];
                    const auto vertex_index = static_cast<size_t>(idx.vertex_index);
                    tinyobj::real_t const vx = attrib.vertices[(3 * vertex_index) + 0];
                    tinyobj::real_t const vy = attrib.vertices[(3 * vertex_index) + 1];
                    tinyobj::real_t const vz = attrib.vertices[(3 * vertex_index) + 2];
                    glm::vec3 const pos = { vx, vy, vz };

                    glm::vec3 normals(0.0F);
                    // A negative normal_index means no normal data.
                    if (idx.normal_index >= 0
                        && (3 * static_cast<size_t>(idx.normal_index)) + 2 < attrib.normals.size()) {
                        tinyobj::real_t const nx = attrib.normals[(3 * static_cast<size_t>(idx.normal_index)) + 0];
                        tinyobj::real_t const ny = attrib.normals[(3 * static_cast<size_t>(idx.normal_index)) + 1];
                        tinyobj::real_t const nz = attrib.normals[(3 * static_cast<size_t>(idx.normal_index)) + 2];
                        normals = glm::vec3(nx, ny, nz);
                    }

                    // Shaders multiply this in like COLOR_0, so the absent case must be white.
                    glm::vec4 color(1.F);
                    if ((3 * vertex_index) + 2 < attrib.colors.size()) {
                        tinyobj::real_t const red = attrib.colors[(3 * vertex_index) + 0];
                        tinyobj::real_t const green = attrib.colors[(3 * vertex_index) + 1];
                        tinyobj::real_t const blue = attrib.colors[(3 * vertex_index) + 2];
                        color = glm::vec4(red, green, blue, 1.F);
                    }

                    glm::vec2 tex_coords(0.0F);
                    // A negative texcoord_index means no texcoord data.
                    if (idx.texcoord_index >= 0
                        && (2 * static_cast<size_t>(idx.texcoord_index)) + 1 < attrib.texcoords.size()) {
                        tinyobj::real_t const tx =
                          attrib.texcoords[(2 * static_cast<size_t>(idx.texcoord_index)) + 0];
                        // flip y coordinate !!
                        tinyobj::real_t const ty =
                          1.F - attrib.texcoords[(2 * static_cast<size_t>(idx.texcoord_index)) + 1];
                        tex_coords = glm::vec2(tx, ty);
                    }

                    Vertex const vert{ pos, normals, color, tex_coords };

                    // One hash lookup per vertex.
                    const auto [entry, inserted] =
                      vertices_map.try_emplace(vert, static_cast<uint32_t>(vertices.size()));
                    if (inserted) { vertices.push_back(vert); }
                    indices.push_back(entry->second);
                }

                // tinyobj reports -1 without a material; cast, that reads out of bounds on the GPU, so use slot 0.
                const int face_material = shape.mesh.material_ids[f];
                materialIndex.push_back(face_material >= 0 ? static_cast<uint32_t>(face_material) : 0U);
            }

            index_offset += fv;
        }

        // A shape whose every face was dropped gets no range, so no empty mesh is built.
        if (vertices.size() > shape_vertex_base) {
            meshRanges.push_back(MeshRange{ shape_vertex_base,
                                                       vertices.size() - shape_vertex_base,
                                                       shape_index_start,
                                                       indices.size() - shape_index_start,
                                                       shape_tri_start,
                                                       materialIndex.size() - shape_tri_start });
        }
    }

    // Fills only zero normals, so files with partial `vn` keep theirs.
    vertex::fillMissingFlatNormals(vertices, indices);

    // OBJ has no tangents; this needs the final normals, so it runs last.
    vertex::computeTangents(vertices, indices);
}
