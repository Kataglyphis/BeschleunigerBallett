// Both glTF parsers (cgltf and OxidANT's Rust loader) must agree; a mismatch is listed by file and field.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "ScopedEnvVar.hpp"

import kataglyphis.vulkan.gltf_loader;
import kataglyphis.vulkan.obj_material;
import kataglyphis.vulkan.punctual_light;
import kataglyphis.vulkan.scene_config;
import kataglyphis.vulkan.vertex;

namespace fs = std::filesystem;

namespace {

// Rust bakes world space in f32 like glm does, but sums and normalizes in another order.
constexpr float kPositionTolerance = 1e-4F;
constexpr float kDirectionTolerance = 1e-3F;
constexpr float kValueTolerance = 1e-5F;

/// Mismatches by class ("file: field"), each with its count and first example.
class MismatchLog
{
  public:
    void add(const std::string &cls, const std::string &example)
    {
        Entry &entry = entries[cls];
        if (entry.count++ == 0) { entry.example = example; }
    }

    [[nodiscard]] bool empty() const { return entries.empty(); }

    [[nodiscard]] std::string report() const
    {
        std::ostringstream out;
        for (const auto &[cls, entry] : entries) {
            out << "\n  " << cls << " x" << entry.count << " (first: " << entry.example << ")";
        }
        return out.str();
    }

  private:
    struct Entry
    {
        std::size_t count = 0;
        std::string example;
    };
    std::map<std::string, Entry> entries;
};

template<typename V> bool near(const V &a, const V &b, float tolerance)
{
    for (int i = 0; i < V::length(); ++i) {
        if (!(std::abs(a[i] - b[i]) <= tolerance)) { return false; }
    }
    return true;
}

template<typename V> std::string show(const V &v)
{
    std::ostringstream out;
    out << "(";
    for (int i = 0; i < V::length(); ++i) { out << (i > 0 ? ", " : "") << v[i]; }
    out << ")";
    return out.str();
}

std::string at(std::size_t index, const std::string &cgltf, const std::string &rust)
{ return "[" + std::to_string(index) + "] cgltf " + cgltf + " rust " + rust; }

template<typename V>
void compareVec(MismatchLog &log,
  const std::string &cls,
  std::size_t index,
  const V &cgltf,
  const V &rust,
  float tolerance)
{
    if (!near(cgltf, rust, tolerance)) { log.add(cls, at(index, show(cgltf), show(rust))); }
}

void compareFloat(MismatchLog &log, const std::string &cls, std::size_t index, float cgltf, float rust)
{
    if (!(std::abs(cgltf - rust) <= kValueTolerance)) {
        log.add(cls, at(index, std::to_string(cgltf), std::to_string(rust)));
    }
}

void compareInt(MismatchLog &log, const std::string &cls, std::size_t index, long long cgltf, long long rust)
{
    if (cgltf != rust) { log.add(cls, at(index, std::to_string(cgltf), std::to_string(rust))); }
}

/// Per vertex, the in-surface part of its UV0 tangent sum over the sum of the terms' lengths: near zero where mirrored
/// UVs cancel, so the Lengyel tangent's direction is rounding noise, not a loader's choice.
std::vector<float> tangentConditioning(const std::vector<Vertex> &vertices, const std::vector<unsigned int> &indices)
{
    std::vector<glm::vec3> sum(vertices.size(), glm::vec3(0.0F));
    std::vector<float> magnitude(vertices.size(), 0.0F);
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const Vertex &v0 = vertices[indices[i]];
        const Vertex &v1 = vertices[indices[i + 1]];
        const Vertex &v2 = vertices[indices[i + 2]];
        const glm::vec2 d1 = v1.texture_coords - v0.texture_coords;
        const glm::vec2 d2 = v2.texture_coords - v0.texture_coords;
        const float det = (d1.x * d2.y) - (d2.x * d1.y);
        if (std::abs(det) < 1e-8F) { continue; }
        const glm::vec3 t = ((v1.position - v0.position) * d2.y - (v2.position - v0.position) * d1.y) / det;
        for (std::size_t k = 0; k < 3; ++k) {
            sum[indices[i + k]] += t;
            magnitude[indices[i + k]] += glm::length(t);
        }
    }
    std::vector<float> conditioning(vertices.size(), 1.0F);
    for (std::size_t v = 0; v < vertices.size(); ++v) {
        const glm::vec3 n = glm::normalize(vertices[v].normal);
        if (magnitude[v] > 0.0F) { conditioning[v] = glm::length(sum[v] - n * glm::dot(n, sum[v])) / magnitude[v]; }
    }
    return conditioning;
}

// Below this a corner's opposing UV triangles cancel to the last few bits.
constexpr float kIllConditionedTangent = 1e-3F;

void compareVertices(MismatchLog &log,
  MismatchLog &known,
  const std::string &file,
  const Kataglyphis::GltfLoader &cgltfLoader,
  const std::vector<Vertex> &rust)
{
    const std::vector<Vertex> &cgltf = cgltfLoader.getVertices();
    compareInt(
      log, file + ": vertex count", 0, static_cast<long long>(cgltf.size()), static_cast<long long>(rust.size()));
    if (cgltf.size() != rust.size()) { return; }
    std::vector<float> conditioning;
    for (std::size_t i = 0; i < cgltf.size(); ++i) {
        const Vertex &c = cgltf[i];
        const Vertex &r = rust[i];
        compareVec(log, file + ": vertex position", i, c.position, r.position, kPositionTolerance);
        compareVec(log, file + ": vertex normal", i, c.normal, r.normal, kDirectionTolerance);
        compareVec(log, file + ": vertex color", i, c.color, r.color, kValueTolerance);
        compareVec(log, file + ": vertex uv0", i, c.texture_coords, r.texture_coords, kValueTolerance);
        compareVec(log, file + ": vertex uv1", i, c.texture_coords1, r.texture_coords1, kValueTolerance);
        const bool tangentsAgree =
          near(glm::vec3(c.tangent), glm::vec3(r.tangent), kDirectionTolerance) && c.tangent.w == r.tangent.w;
        if (tangentsAgree) { continue; }
        if (conditioning.empty()) { conditioning = tangentConditioning(cgltf, cgltfLoader.getIndices()); }
        if (conditioning[i] < kIllConditionedTangent) {
            known.add("generated tangent where mirrored UVs cancel: direction is rounding noise", file);
            continue;
        }
        compareVec(
          log, file + ": vertex tangent xyz", i, glm::vec3(c.tangent), glm::vec3(r.tangent), kDirectionTolerance);
        compareFloat(log, file + ": vertex tangent w", i, c.tangent.w, r.tangent.w);
    }
}

void compareIndexArrays(MismatchLog &log,
  const std::string &cls,
  const std::vector<unsigned int> &cgltf,
  const std::vector<unsigned int> &rust)
{
    compareInt(log, cls + " count", 0, static_cast<long long>(cgltf.size()), static_cast<long long>(rust.size()));
    for (std::size_t i = 0; i < std::min(cgltf.size(), rust.size()); ++i) {
        compareInt(log, cls, i, cgltf[i], rust[i]);
    }
}

void compareMaterial(MismatchLog &log,
  const std::string &file,
  std::size_t i,
  const ObjMaterial &c,
  const ObjMaterial &r)
{
    const std::string m = file + ": material ";
    compareVec(log, m + "diffuse", i, c.diffuse, r.diffuse, kValueTolerance);
    compareVec(log, m + "emission", i, c.emission, r.emission, kValueTolerance);
    compareFloat(log, m + "shininess", i, c.shininess, r.shininess);
    compareFloat(log, m + "dissolve", i, c.dissolve, r.dissolve);
    compareFloat(log, m + "alphaCutoff", i, c.alphaCutoff, r.alphaCutoff);
    compareFloat(log, m + "metallic", i, c.metallic, r.metallic);
    compareFloat(log, m + "roughness", i, c.roughness, r.roughness);
    compareFloat(log, m + "normalScale", i, c.normalScale, r.normalScale);
    compareInt(log, m + "textureID", i, c.textureID, r.textureID);
    compareInt(log, m + "metallicRoughnessTextureID", i, c.metallicRoughnessTextureID, r.metallicRoughnessTextureID);
    compareInt(log, m + "normalTextureID", i, c.normalTextureID, r.normalTextureID);
    compareInt(log, m + "emissiveTextureID", i, c.emissiveTextureID, r.emissiveTextureID);
    compareInt(log, m + "alphaTextureID", i, c.alphaTextureID, r.alphaTextureID);
    compareInt(log, m + "unlit", i, c.unlit, r.unlit);
    compareInt(log, m + "uv1SlotMask", i, c.uv1SlotMask, r.uv1SlotMask);
    compareInt(log, m + "alphaBlend", i, c.alphaBlend, r.alphaBlend);
    compareVec(log, m + "uv_transform_row0", i, c.uv_transform_row0, r.uv_transform_row0, kValueTolerance);
    compareVec(log, m + "uv_transform_row1", i, c.uv_transform_row1, r.uv_transform_row1, kValueTolerance);
    compareVec(
      log, m + "normal_uv_transform_row0", i, c.normal_uv_transform_row0, r.normal_uv_transform_row0, kValueTolerance);
    compareVec(
      log, m + "normal_uv_transform_row1", i, c.normal_uv_transform_row1, r.normal_uv_transform_row1, kValueTolerance);
    compareVec(log,
      m + "metallic_roughness_uv_transform_row0",
      i,
      c.metallic_roughness_uv_transform_row0,
      r.metallic_roughness_uv_transform_row0,
      kValueTolerance);
    compareVec(log,
      m + "metallic_roughness_uv_transform_row1",
      i,
      c.metallic_roughness_uv_transform_row1,
      r.metallic_roughness_uv_transform_row1,
      kValueTolerance);
    compareVec(log,
      m + "emissive_uv_transform_row0",
      i,
      c.emissive_uv_transform_row0,
      r.emissive_uv_transform_row0,
      kValueTolerance);
    compareVec(log,
      m + "emissive_uv_transform_row1",
      i,
      c.emissive_uv_transform_row1,
      r.emissive_uv_transform_row1,
      kValueTolerance);
}

void compareMaterials(MismatchLog &log,
  const std::string &file,
  const std::vector<ObjMaterial> &cgltf,
  const std::vector<ObjMaterial> &rust)
{
    compareInt(
      log, file + ": material count", 0, static_cast<long long>(cgltf.size()), static_cast<long long>(rust.size()));
    for (std::size_t i = 0; i < std::min(cgltf.size(), rust.size()); ++i) {
        compareMaterial(log, file, i, cgltf[i], rust[i]);
    }
}

void compareRanges(MismatchLog &log,
  const std::string &file,
  const std::vector<Kataglyphis::MeshRange> &cgltf,
  const std::vector<Kataglyphis::MeshRange> &rust)
{
    const std::string m = file + ": mesh range ";
    compareInt(log, m + "count", 0, static_cast<long long>(cgltf.size()), static_cast<long long>(rust.size()));
    for (std::size_t i = 0; i < std::min(cgltf.size(), rust.size()); ++i) {
        const Kataglyphis::MeshRange &c = cgltf[i];
        const Kataglyphis::MeshRange &r = rust[i];
        compareInt(
          log, m + "vertexBase", i, static_cast<long long>(c.vertexBase), static_cast<long long>(r.vertexBase));
        compareInt(
          log, m + "vertexCount", i, static_cast<long long>(c.vertexCount), static_cast<long long>(r.vertexCount));
        compareInt(
          log, m + "indexStart", i, static_cast<long long>(c.indexStart), static_cast<long long>(r.indexStart));
        compareInt(
          log, m + "indexCount", i, static_cast<long long>(c.indexCount), static_cast<long long>(r.indexCount));
        compareInt(log, m + "triStart", i, static_cast<long long>(c.triStart), static_cast<long long>(r.triStart));
        compareInt(log, m + "triCount", i, static_cast<long long>(c.triCount), static_cast<long long>(r.triCount));
        compareInt(log, m + "doubleSided", i, c.doubleSided, r.doubleSided);
        compareInt(log, m + "alphaBlended", i, c.alphaBlended, r.alphaBlended);
    }
}

void compareTextures(MismatchLog &log,
  const std::string &file,
  const Kataglyphis::GltfLoader &c,
  const Kataglyphis::GltfLoader &r)
{
    const std::string m = file + ": texture ";
    const auto &cImages = c.getTextureImages();
    const auto &rImages = r.getTextureImages();
    compareInt(log, m + "count", 0, static_cast<long long>(cImages.size()), static_cast<long long>(rImages.size()));
    for (std::size_t i = 0; i < std::min(cImages.size(), rImages.size()); ++i) {
        if (cImages[i] != rImages[i]) {
            log.add(
              m + "bytes", at(i, std::to_string(cImages[i].size()) + " B", std::to_string(rImages[i].size()) + " B"));
        }
        if (!(c.getTextureSamplerDescs()[i] == r.getTextureSamplerDescs()[i])) {
            log.add(m + "sampler", at(i, "", ""));
        }
        compareInt(log, m + "srgb", i, c.getTextureSrgbFlags()[i], r.getTextureSrgbFlags()[i]);
    }
}

void compareLights(MismatchLog &log,
  const std::string &file,
  const std::vector<Kataglyphis::PunctualLight> &cgltf,
  const std::vector<Kataglyphis::PunctualLight> &rust)
{
    const std::string m = file + ": light ";
    compareInt(log, m + "count", 0, static_cast<long long>(cgltf.size()), static_cast<long long>(rust.size()));
    for (std::size_t i = 0; i < std::min(cgltf.size(), rust.size()); ++i) {
        const Kataglyphis::PunctualLight &c = cgltf[i];
        const Kataglyphis::PunctualLight &r = rust[i];
        compareInt(log, m + "kind", i, static_cast<long long>(c.kind), static_cast<long long>(r.kind));
        compareVec(log, m + "color", i, c.color, r.color, kValueTolerance);
        compareFloat(log, m + "intensity", i, c.intensity, r.intensity);
        compareFloat(log, m + "range", i, c.range, r.range);
        compareVec(log, m + "position", i, c.position, r.position, kPositionTolerance);
        compareVec(log, m + "direction", i, c.direction, r.direction, kDirectionTolerance);
        compareFloat(log, m + "cosInner", i, c.cosInner, r.cosInner);
        compareFloat(log, m + "cosOuter", i, c.cosOuter, r.cosOuter);
    }
}

/// Parses `path` with both loaders and logs every field that differs.
void compareFile(MismatchLog &log, MismatchLog &known, const fs::path &path)
{
    const std::string file = path.filename().string();
    Kataglyphis::GltfLoader cgltf;
    Kataglyphis::GltfLoader rust;
    const bool cgltfOk = cgltf.parseCpu(path.string(), Kataglyphis::GltfParser::Cgltf);
    const bool rustOk = rust.parseCpu(path.string(), Kataglyphis::GltfParser::Rust);
    compareInt(log, file + ": parse result", 0, cgltfOk ? 1 : 0, rustOk ? 1 : 0);
    if (!cgltfOk || !rustOk) { return; }
    compareVertices(log, known, file, cgltf, rust.getVertices());
    compareIndexArrays(log, file + ": indices", cgltf.getIndices(), rust.getIndices());
    compareIndexArrays(log, file + ": materialIndex", cgltf.getMaterialIndices(), rust.getMaterialIndices());
    compareMaterials(log, file, cgltf.getMaterials(), rust.getMaterials());
    compareRanges(log, file, cgltf.getMeshRanges(), rust.getMeshRanges());
    compareTextures(log, file, cgltf, rust);
    compareLights(log, file, cgltf.getPunctualLights(), rust.getPunctualLights());
}

fs::path modelsRoot()
{ return fs::path(sceneConfig::resolveModelPath("Models/GltfTest/cube.glb")).parent_path().parent_path(); }

/// Every .gltf and .glb under `dir`, sorted; `skip` leaves out one subdirectory.
std::vector<fs::path> gltfFilesUnder(const fs::path &dir, const fs::path &skip = {})
{
    std::vector<fs::path> files;
    std::error_code error;
    for (fs::recursive_directory_iterator it(dir, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!skip.empty() && it->path() == skip) {
            it.disable_recursion_pending();
            continue;
        }
        const std::string ext = it->path().extension().string();
        if (it->is_regular_file(error) && (ext == ".gltf" || ext == ".glb")) { files.push_back(it->path()); }
    }
    std::sort(files.begin(), files.end());
    return files;
}

}// namespace

TEST(GltfLoaderDiff, EveryGltfTestFixtureParsesTheSameWithBothLoaders)
{
    const fs::path fixtures = modelsRoot() / "GltfTest";
    const std::vector<fs::path> files = gltfFilesUnder(fixtures);
    if (files.empty()) { GTEST_SKIP() << "no glTF fixtures under " << fixtures; }

    MismatchLog log;
    MismatchLog known;
    for (const fs::path &file : files) { compareFile(log, known, file); }
    EXPECT_TRUE(log.empty()) << files.size() << " fixtures; mismatches by file and field:" << log.report();
    std::cout << "Known divergences:" << known.report() << "\n";
}

TEST(GltfLoaderDiff, FetchedSceneGltfsParseTheSameWithBothLoaders)
{
    // Dinosaurs and crytek-sponza are OBJ; KATAGLYPHIS_FETCH_ALL_ASSETS fetches the glTF scenes.
    const fs::path models = modelsRoot();
    const std::vector<fs::path> files = gltfFilesUnder(models, models / "GltfTest");
    if (files.empty()) { GTEST_SKIP() << "no fetched glTF scene under " << models; }

    MismatchLog log;
    MismatchLog known;
    for (const fs::path &file : files) { compareFile(log, known, file); }
    EXPECT_TRUE(log.empty()) << files.size() << " scenes; mismatches by file and field:" << log.report();
    std::cout << "Known divergences:" << known.report() << "\n";
}

// The Rust loader is the default since 2026-11-18; only an explicit cgltf keeps the old parser.
TEST(GltfLoaderDiff, TheEnvironmentPicksRustUnlessItNamesCgltf)
{
    using Kataglyphis::GltfParser;
    using Kataglyphis::TestSupport::ScopedEnvVar;
    {
        const ScopedEnvVar unset("KATAGLYPHIS_GLTF_LOADER", "");
        EXPECT_EQ(Kataglyphis::gltfParserFromEnvironment(), GltfParser::Rust);
    }
    {
        const ScopedEnvVar rust("KATAGLYPHIS_GLTF_LOADER", "rust");
        EXPECT_EQ(Kataglyphis::gltfParserFromEnvironment(), GltfParser::Rust);
    }
    {
        const ScopedEnvVar other("KATAGLYPHIS_GLTF_LOADER", "tinygltf");
        EXPECT_EQ(Kataglyphis::gltfParserFromEnvironment(), GltfParser::Rust);
    }
    {
        const ScopedEnvVar cgltf("KATAGLYPHIS_GLTF_LOADER", "cgltf");
        EXPECT_EQ(Kataglyphis::gltfParserFromEnvironment(), GltfParser::Cgltf);
    }
}
