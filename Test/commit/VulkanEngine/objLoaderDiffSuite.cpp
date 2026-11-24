// Differential suite: tinyobjloader's ObjLoader and OxidANT's OBJ reader (over the cxx bridge, through GltfLoader)
// on every OBJ fixture and fetched scene. The two split a file into meshes differently (shape against material), so
// triangles are matched by their corner positions and compared corner by corner, with their materials.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "RepoFiles.hpp"

import kataglyphis.vulkan.gltf_loader;
import kataglyphis.vulkan.obj_loader;
import kataglyphis.vulkan.obj_material;
import kataglyphis.vulkan.vertex;

namespace fs = std::filesystem;
using Kataglyphis::TestSupport::oxidantGltfFixtureDir;
using Kataglyphis::TestSupport::readFileText;
using Kataglyphis::TestSupport::repoRoot;

namespace {

constexpr float kValueTolerance = 1e-5F;
constexpr float kUvTolerance = 1e-5F;
constexpr float kNormalDot = 0.9999F;
constexpr float kTangentDot = 0.999F;

/// Mismatches by class, each with its count and first example.
class MismatchLog
{
  public:
    void add(const std::string &cls, const std::string &example)
    {
        Entry &entry = entries[cls];
        if (entry.count++ == 0) { entry.example = example; }
    }

    [[nodiscard]] std::set<std::string> classes() const
    {
        std::set<std::string> names;
        for (const auto &[cls, entry] : entries) { names.insert(cls); }
        return names;
    }

    [[nodiscard]] std::string report() const
    {
        std::ostringstream out;
        for (const auto &[cls, entry] : entries) {
            out << "\n    " << cls << " x" << entry.count << " (first: " << entry.example << ")";
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

template<typename V> std::string show(const V &v)
{
    std::ostringstream out;
    out << "(";
    for (int i = 0; i < V::length(); ++i) { out << (i > 0 ? ", " : "") << v[i]; }
    out << ")";
    return out.str();
}

template<typename V> bool near(const V &a, const V &b, float tolerance)
{
    for (int i = 0; i < V::length(); ++i) {
        if (!(std::abs(a[i] - b[i]) <= tolerance * std::max(1.0F, std::abs(a[i])))) { return false; }
    }
    return true;
}

/// The roughness a shader reads: `material_rules.slang`'s `material_roughness()`.
float effectiveRoughness(const ObjMaterial &m)
{
    const float value = m.roughness >= 0.0F ? m.roughness : std::sqrt(2.0F / (m.shininess + 2.0F));
    return std::clamp(value, 0.045F, 1.0F);
}

std::optional<std::size_t> hashOf(std::span<const unsigned char> bytes)
{
    if (bytes.empty()) { return std::nullopt; }
    return std::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
}

std::optional<std::size_t> hashOfFile(const std::string &path)
{
    const std::optional<std::string> text = readFileText(path);
    if (!text || text->empty()) { return std::nullopt; }
    return std::hash<std::string_view>{}(*text);
}

/// A texture slot as both loaders can be compared on: the file's bytes and colour space; none for -1 or no file.
struct Slot
{
    std::optional<std::size_t> hash;
    bool srgb = false;
    bool operator==(const Slot &) const = default;
};

std::vector<Slot> cppSlots(const Kataglyphis::ObjLoader &loader)
{
    std::vector<Slot> slots;
    const auto &names = loader.getTextureNames();
    const auto &srgb = loader.getTextureSrgbFlags();
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (!names[i].empty()) { slots.push_back({ hashOfFile(names[i]), srgb[i] != 0 }); }
    }
    return slots;
}

std::vector<Slot> rustSlots(const Kataglyphis::GltfLoader &loader)
{
    std::vector<Slot> slots;
    for (std::size_t i = 0; i < loader.getTextureImages().size(); ++i) {
        slots.push_back({ hashOf(loader.getTextureImages()[i]), loader.getTextureSrgbFlags()[i] != 0 });
    }
    return slots;
}

Slot slotAt(const std::vector<Slot> &slots, int id)
{
    if (id < 0 || static_cast<std::size_t>(id) >= slots.size() || !slots[static_cast<std::size_t>(id)].hash) {
        return {};
    }
    return slots[static_cast<std::size_t>(id)];
}

std::string slotText(const Slot &slot) { return slot.hash ? (slot.srgb ? "srgb file" : "linear file") : "none"; }

struct Side
{
    std::span<const Vertex> vertices;
    std::span<const unsigned int> indices;
    std::span<const unsigned int> materialIndex;
    std::span<const ObjMaterial> materials;
    std::vector<Slot> slots;
};

/// A triangle's corner positions on a grid fine enough to separate any two real corners.
struct Key
{
    std::array<std::int64_t, 9> cell{};
    bool operator==(const Key &) const = default;
};

struct KeyHash
{
    std::size_t operator()(const Key &key) const
    {
        std::size_t h = 0;
        for (const std::int64_t c : key.cell) { h = (h * 1000003U) ^ std::hash<std::int64_t>{}(c); }
        return h;
    }
};

Key keyOf(const Side &side, std::size_t tri, float cell)
{
    Key key;
    for (std::size_t k = 0; k < 3; ++k) {
        const glm::vec3 p = side.vertices[side.indices[(3 * tri) + k]].position;
        for (int axis = 0; axis < 3; ++axis) { key.cell[(3 * k) + axis] = std::llround(p[axis] / cell); }
    }
    return key;
}

bool sameSurface(const Side &cpp, const Side &rust, std::size_t cTri, std::size_t rTri, bool normals)
{
    const ObjMaterial &c = cpp.materials[cpp.materialIndex[cTri]];
    const ObjMaterial &r = rust.materials[rust.materialIndex[rTri]];
    if (!near(c.diffuse, r.diffuse, kValueTolerance)) { return false; }
    for (std::size_t k = 0; k < 3; ++k) {
        const glm::vec2 a = cpp.vertices[cpp.indices[(3 * cTri) + k]].texture_coords;
        const glm::vec2 b = rust.vertices[rust.indices[(3 * rTri) + k]].texture_coords;
        if (!near(a, b, kUvTolerance)) { return false; }
        const glm::vec3 na = glm::normalize(cpp.vertices[cpp.indices[(3 * cTri) + k]].normal);
        const glm::vec3 nb = glm::normalize(rust.vertices[rust.indices[(3 * rTri) + k]].normal);
        if (normals && !(glm::dot(na, nb) >= kNormalDot)) { return false; }
    }
    return true;
}

void compareMaterial(MismatchLog &log, const Side &cpp, const Side &rust, std::size_t cTri, std::size_t rTri)
{
    const ObjMaterial &c = cpp.materials[cpp.materialIndex[cTri]];
    const ObjMaterial &r = rust.materials[rust.materialIndex[rTri]];
    const auto vec = [&](const char *cls, const glm::vec3 &a, const glm::vec3 &b) {
        if (!near(a, b, kValueTolerance)) { log.add(cls, "cpp " + show(a) + " rust " + show(b)); }
    };
    const auto num = [&](const char *cls, float a, float b) {
        if (!(std::abs(a - b) <= kValueTolerance)) {
            log.add(cls, "cpp " + std::to_string(a) + " rust " + std::to_string(b));
        }
    };
    vec("material diffuse", c.diffuse, r.diffuse);
    vec("material emission", c.emission, r.emission);
    num("material dissolve", c.dissolve, r.dissolve);
    num("material metallic", c.metallic, r.metallic);
    num("material roughness (as shaded)", effectiveRoughness(c), effectiveRoughness(r));
    num("material alphaCutoff", c.alphaCutoff, r.alphaCutoff);
    num("material normalScale", c.normalScale, r.normalScale);
    num("material alphaBlend", static_cast<float>(c.alphaBlend), static_cast<float>(r.alphaBlend));
    const auto tex = [&](const char *cls, int a, int b) {
        const Slot sa = slotAt(cpp.slots, a);
        const Slot sb = slotAt(rust.slots, b);
        if (!(sa == sb)) { log.add(cls, "cpp " + slotText(sa) + " rust " + slotText(sb)); }
    };
    tex("texture base colour", c.textureID, r.textureID);
    tex("texture normal", c.normalTextureID, r.normalTextureID);
    tex("texture emissive", c.emissiveTextureID, r.emissiveTextureID);
    tex("texture alpha", c.alphaTextureID, r.alphaTextureID);
}

void compareCorner(MismatchLog &log, const Vertex &c, const Vertex &r)
{
    if (!near(c.position, r.position, kValueTolerance)) {
        log.add("corner position", "cpp " + show(c.position) + " rust " + show(r.position));
    }
    // tinyobjloader kept a file's unnormalized vn, OxidANT normalizes it; the shaders normalize either.
    if (!(glm::dot(glm::normalize(c.normal), glm::normalize(r.normal)) >= kNormalDot)) {
        log.add("corner normal", "cpp " + show(c.normal) + " rust " + show(r.normal));
    }
    if (!near(c.texture_coords, r.texture_coords, kUvTolerance)) {
        log.add("corner uv0", "cpp " + show(c.texture_coords) + " rust " + show(r.texture_coords));
    }
    if (!near(c.texture_coords1, r.texture_coords1, kUvTolerance)) {
        log.add("corner uv1", "cpp " + show(c.texture_coords1) + " rust " + show(r.texture_coords1));
    }
    if (!near(c.color, r.color, kValueTolerance)) {
        log.add("corner color", "cpp " + show(c.color) + " rust " + show(r.color));
    }
    if (!(glm::dot(glm::vec3(c.tangent), glm::vec3(r.tangent)) >= kTangentDot)) {
        log.add("corner tangent", "cpp " + show(c.tangent) + " rust " + show(r.tangent));
    }
    if (c.tangent.w != r.tangent.w) {
        log.add("corner tangent handedness", "cpp " + show(c.tangent) + " rust " + show(r.tangent));
    }
}

struct Summary
{
    std::size_t cppTriangles = 0;
    std::size_t rustTriangles = 0;
    std::size_t cppRanges = 0;
    std::size_t rustRanges = 0;
    std::size_t cppVertices = 0;
    std::size_t rustVertices = 0;
};

/// Parses `path` with both loaders and logs every field that differs on matched triangles.
Summary compareFile(MismatchLog &log, const fs::path &path)
{
    Summary summary;
    Kataglyphis::ObjLoader tiny;
    Kataglyphis::GltfLoader bridged;
    const bool tinyOk = tiny.parseCpu(path.string());
    const bool bridgedOk = bridged.parseCpu(path.string());
    if (tinyOk != bridgedOk) { log.add("parse result", tinyOk ? "only cpp parsed" : "only rust parsed"); }
    if (!tinyOk || !bridgedOk) { return summary; }

    const Side cpp{
        tiny.getVertices(), tiny.getIndices(), tiny.getMaterialIndices(), tiny.getMaterials(), cppSlots(tiny)
    };
    const Side rust{ bridged.getVertices(),
        bridged.getIndices(),
        bridged.getMaterialIndices(),
        bridged.getMaterials(),
        rustSlots(bridged) };
    summary = { cpp.indices.size() / 3,
        rust.indices.size() / 3,
        tiny.getMeshRanges().size(),
        bridged.getMeshRanges().size(),
        cpp.vertices.size(),
        rust.vertices.size() };
    if (summary.cppTriangles != summary.rustTriangles) {
        log.add("triangle count",
          "cpp " + std::to_string(summary.cppTriangles) + " rust " + std::to_string(summary.rustTriangles));
    }

    glm::vec3 lo(INFINITY);
    glm::vec3 hi(-INFINITY);
    for (const Vertex &v : cpp.vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
    const float cell = std::max(1e-6F * glm::length(hi - lo), 1e-12F);

    std::unordered_multimap<Key, std::size_t, KeyHash> rustByKey;
    rustByKey.reserve(summary.rustTriangles);
    for (std::size_t t = 0; t < summary.rustTriangles; ++t) { rustByKey.emplace(keyOf(rust, t, cell), t); }
    std::vector<unsigned char> used(summary.rustTriangles, 0);
    for (std::size_t t = 0; t < summary.cppTriangles; ++t) {
        const auto [first, last] = rustByKey.equal_range(keyOf(cpp, t, cell));
        // Coincident triangles (a liquid inside its glass, doubled walls) pair by material, UVs and normals first.
        std::optional<std::size_t> match;
        for (const bool normals : { true, false }) {
            for (auto it = first; it != last && !match; ++it) {
                if (used[it->second] == 0 && sameSurface(cpp, rust, t, it->second, normals)) { match = it->second; }
            }
        }
        for (auto it = first; it != last && !match; ++it) {
            if (used[it->second] == 0) { match = it->second; }
        }
        if (!match) {
            log.add("triangle without a counterpart", "cpp triangle " + std::to_string(t));
            continue;
        }
        used[*match] = 1;
        for (std::size_t k = 0; k < 3; ++k) {
            compareCorner(log, cpp.vertices[cpp.indices[(3 * t) + k]], rust.vertices[rust.indices[(3 * *match) + k]]);
        }
        compareMaterial(log, cpp, rust, t, *match);
    }
    return summary;
}

/// Every .obj under the models folder and OxidANT's OBJ fixtures, sorted.
std::vector<fs::path> objFiles()
{
    std::vector<fs::path> files;
    for (const fs::path &dir : { repoRoot() / "Resources" / "Models", oxidantGltfFixtureDir() / "obj_mask" }) {
        std::error_code error;
        for (fs::recursive_directory_iterator it(dir, error), end; it != end; it.increment(error)) {
            if (error) { break; }
            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
            if (ext == ".obj" && it->is_regular_file(error)) { files.push_back(it->path()); }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

// The differences the switch keeps, each by file and class with its reason; any other class fails.
constexpr const char *kWhite =
  "no Kd, no .mtl or an undeclared usemtl is white (owner 2026-11-19; was 0.7 or material 0)";
constexpr const char *kRough =
  "no Pr and no Ns is roughness 1 (owner 2026-11-19; tinyobjloader's Ns default 1 gave 0.816)";
constexpr const char *kBlend = "d below 1 or Tr above 0 is BLEND (owner 2026-11-19; ObjLoader drew it opaque)";
constexpr const char *kFused =
  "aarch64 clang fuses tinyobjloader's quad-diagonal sums; near-equal diagonals split the other way, and the tangents "
  "around them follow (the clang-cl x64 build does not fuse)";
constexpr const char *kPerFace =
  "a corner without vn takes its own face's flat normal (review 2026-11-24, as glTF unindexes such a primitive); "
  "ObjLoader shared it between faces of one group, so it took another face's normal and tangent";
constexpr const char *kMerged =
  "ObjLoader merged equal-valued vertices, so a corner with a zero vn took another face's flat normal and tangent sums "
  "spanned more triangles; the reader keeps a file's own vertices";

const std::map<std::string, std::map<std::string, const char *>> &intendedDifferences()
{
    static const std::map<std::string, std::map<std::string, const char *>> table = {
        { "CornellBox-Glossy-Floor.obj",
          { { "material diffuse", kWhite }, { "material roughness (as shaded)", kWhite } } },
        { "CornellBox-Glossy.obj", { { "material diffuse", kWhite }, { "material roughness (as shaded)", kWhite } } },
        { "CornellBox-Mirror.obj", { { "corner normal", kPerFace }, { "corner tangent", kPerFace } } },
        { "CornellBox-Original.obj", { { "corner normal", kPerFace }, { "corner tangent", kPerFace } } },
        { "water.obj", { { "corner normal", kPerFace }, { "corner tangent", kPerFace } } },
        { "shadow_rig.obj", { { "material diffuse", kWhite } } },
        { "dragon.obj", { { "material diffuse", kWhite } } },
        { "buddha.obj",
          { { "material diffuse", kWhite }, { "corner normal", kMerged }, { "corner tangent", kMerged } } },
        { "bunny.obj", { { "material diffuse", kWhite } } },
        { "SuloLongDongLampe.obj",
          { { "material alphaBlend", kBlend },
            { "triangle without a counterpart", kFused },
            { "corner tangent", kFused } } },
        { "SuloLongDongLampe_v2.obj",
          { { "material diffuse", kWhite }, { "corner normal", kPerFace }, { "corner tangent", kPerFace } } },
        { "SuloLongDongLampe_v2_fix.obj",
          { { "material roughness (as shaded)", kRough },
            { "corner normal", kPerFace },
            { "corner tangent", kPerFace } } },
        { "Weinglas_Vol.obj", { { "triangle without a counterpart", kFused } } },
        { "Wolf-Stahl.obj", { { "triangle without a counterpart", kFused } } },
        { "viking_room.obj", { { "corner tangent", kMerged }, { "corner tangent handedness", kMerged } } },
        { "bmw.obj",
          { { "material alphaBlend", kBlend },
            { "corner normal", kMerged },
            { "corner tangent", kMerged },
            { "corner tangent handedness", kMerged } } },
        { "testObj.obj", { { "material roughness (as shaded)", kRough } } },
        { "sibenik.obj",
          { { "material alphaBlend", kBlend },
            { "corner normal", kPerFace },
            { "corner tangent", kPerFace },
            { "corner tangent handedness", kPerFace } } },
        { "sportsCar.obj", { { "triangle without a counterpart", kFused } } },
        { "card.obj", { { "material roughness (as shaded)", kRough } } },
    };
    return table;
}

}// namespace

TEST(ObjLoaderDiff, EveryObjParsesTheSameWithBothReaders)
{
    const std::vector<fs::path> files = objFiles();
    ASSERT_FALSE(files.empty());
    std::ostringstream unexpected;
    for (const fs::path &file : files) {
        MismatchLog log;
        const Summary s = compareFile(log, file);
        const std::string name = fs::relative(file, repoRoot()).generic_string();
        std::cout << name << ": triangles " << s.cppTriangles << "/" << s.rustTriangles << ", meshes " << s.cppRanges
                  << "/" << s.rustRanges << ", vertices " << s.cppVertices << "/" << s.rustVertices << " (cpp/rust)"
                  << log.report() << "\n";
        const auto known = intendedDifferences().find(file.filename().string());
        if (known != intendedDifferences().end()) {
            for (const auto &[cls, reason] : known->second) {
                if (!log.classes().contains(cls)) { unexpected << "\n  " << name << ": no longer differs in " << cls; }
            }
        }
        for (const std::string &cls : log.classes()) {
            const bool intended = known != intendedDifferences().end() && known->second.contains(cls);
            if (intended) {
                std::cout << "  intended: " << cls << ": " << known->second.at(cls) << "\n";
            } else {
                unexpected << "\n  " << name << ": " << cls;
            }
        }
    }
    EXPECT_TRUE(unexpected.str().empty()) << "unexplained mismatches by file and field:" << unexpected.str();
}
