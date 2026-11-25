// What OxidANT's OBJ reader hands ObjMaterial and MeshRange; GltfLoader{} has no device, so no parse may touch Vulkan.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

import kataglyphis.vulkan.gltf_loader;
import kataglyphis.vulkan.obj_material;
import kataglyphis.vulkan.scene_config;
import kataglyphis.vulkan.scene;

namespace {

namespace fs = std::filesystem;

std::string test_model()
{
    // The purpose-built shadow rig: small, committed, and known-good.
    return sceneConfig::resolveModelPath("Models/ShadowTest/shadow_rig.obj");
}

std::string dinosaurs() { return sceneConfig::resolveModelPath("Models/Dinosaurs/dinosaurs.obj"); }

void writeFile(const fs::path &path, const std::string &text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

// A 1x1 PSD header, the one format OxidANT hands to stb as the file's bytes, so a tag after it names the file.
constexpr std::size_t kPsdHeaderSize = 22;

std::string psdTagged(const std::string &tag)
{
    std::string bytes("8BPS\0\x01\0\0\0\0\0\0\0\x03\0\0\0\x01\0\0\0\x01", kPsdHeaderSize);
    return bytes + tag;
}

/// A fresh directory with `model.mtl` and a textured triangle per material in `materials`, plus `files`.
fs::path writeModel(const std::string &name,
  const std::string &mtl,
  std::initializer_list<const char *> materials,
  std::initializer_list<std::pair<const char *, const char *>> files = {})
{
    const fs::path dir = fs::temp_directory_path() / name;
    fs::remove_all(dir);
    writeFile(dir / "model.mtl", mtl);
    std::string obj = "mtllib model.mtl\nvt 0 0\nvt 1 0\nvt 0 1\n";
    float x = 0.0F;
    for (const char *material : materials) {
        obj += "v " + std::to_string(x) + " 0 0\nv " + std::to_string(x + 1.0F) + " 0 0\nv " + std::to_string(x)
               + " 1 0\nusemtl " + material + "\nf -3/1 -2/2 -1/3\n";
        x += 2.0F;
    }
    writeFile(dir / "model.obj", obj);
    // Non-empty: OxidANT gives an empty file no slot.
    for (const auto &[file, bytes] : files) { writeFile(dir / file, psdTagged(bytes)); }
    return dir;
}

std::string bytesOf(const Kataglyphis::GltfLoader &loader, int slot)
{
    if (slot < 0) { return {}; }
    // A PSD, which OxidANT hands over as the file's bytes for stb; the tag follows its header.
    const std::span<const unsigned char> image = loader.getTextures()[static_cast<std::size_t>(slot)].encoded;
    if (image.size() < kPsdHeaderSize) { return {}; }
    return { image.begin() + static_cast<std::ptrdiff_t>(kPsdHeaderSize), image.end() };
}

}// namespace

TEST(ObjParseUnit, ParsesWithoutAVulkanDevice)
{
    if (!fs::exists(test_model())) { GTEST_SKIP() << "test model not present"; }

    Kataglyphis::GltfLoader loader;// no device, no queue, no command pool
    ASSERT_TRUE(loader.parseCpu(test_model()));

    EXPECT_GT(loader.getVertices().size(), 0U);
    EXPECT_GT(loader.getIndices().size(), 0U);
    EXPECT_EQ(loader.getIndices().size() % 3U, 0U) << "indices must form whole triangles";
}

TEST(ObjParseUnit, FacesWithoutAMaterialIndexInsideTheMaterialsArray)
{
    if (!fs::exists(test_model())) { GTEST_SKIP() << "test model not present"; }

    // shadow_rig.obj names no mtllib; an out-of-range id would make every material fetch read out of bounds.
    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(test_model()));

    ASSERT_FALSE(loader.getMaterials().empty()) << "a file without materials must still yield the default material";
    for (const unsigned int index : loader.getMaterialIndices()) {
        ASSERT_LT(index, loader.getMaterials().size())
          << "face material index escapes the materials array (GPU OOB read)";
    }
}

TEST(ObjParseUnit, AnObjWithoutAnMtlGetsAWhiteNonEmittingMaterial)
{
    // No authored Ke means no emitted radiance; no Kd is glTF's white (owner, 2026-11-19).
    const fs::path tmp = fs::temp_directory_path() / "kat_no_mtllib" / "model.obj";
    writeFile(tmp, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));

    const ObjMaterial &material = loader.getMaterials()[loader.getMaterialIndices()[0]];
    EXPECT_EQ(material.emission, glm::vec3(0.0F));
    EXPECT_EQ(material.diffuse, glm::vec3(1.0F));
    EXPECT_FLOAT_EQ(material.roughness, 1.0F) << "neither Pr nor Ns is roughness 1";

    fs::remove_all(tmp.parent_path());
}

TEST(ObjParseUnit, UntexturedMtlMaterialsRouteToTheDiffuseFallback)
{
    if (!fs::exists(dinosaurs())) { GTEST_SKIP() << "test model not present"; }

    // The shaders' diffuse fallback keys on -1; slot 0 would sample a texture instead of Kd.
    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(dinosaurs()));

    // The materials the triangles draw with; the last one is the neutral fallback no OBJ range uses.
    std::vector<unsigned int> used(loader.getMaterialIndices().begin(), loader.getMaterialIndices().end());
    std::sort(used.begin(), used.end());
    used.erase(std::unique(used.begin(), used.end()), used.end());
    ASSERT_EQ(used.size(), 2U) << "dinosaurs.mtl's None and bone";
    for (const unsigned int id : used) {
        const ObjMaterial &material = loader.getMaterials()[id];
        EXPECT_EQ(material.textureID, -1) << "a material without map_Kd must route to the diffuse fallback";
        EXPECT_EQ(material.emissiveTextureID, -1) << "a material without map_Ke must keep the sentinel";
        EXPECT_NEAR(material.metallic, 0.0F, 1e-6F) << "a .mtl without Pm must default to metallic 0.0";
    }
    EXPECT_TRUE(loader.getTextures().empty()) << "dinosaurs.mtl names no texture";
}

TEST(ObjParseUnit, MtlPbrChannelsReachTheMaterial)
{
    const fs::path dir =
      writeModel("kat_mtl_pbr_channels", "newmtl painted\nKd 1 1 1\nPm 1.0\nPr 0.25\n", { "painted" });

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "model.obj").string()));

    const ObjMaterial &material = loader.getMaterials()[loader.getMaterialIndices()[0]];
    EXPECT_FLOAT_EQ(material.metallic, 1.0F);
    EXPECT_FLOAT_EQ(material.roughness, 0.25F);

    fs::remove_all(dir);
}

TEST(ObjParseUnit, AnMtlWithoutPrTakesItsRoughnessFromNs)
{
    // The shaders' shininess curve, applied once in OxidANT: sqrt(2 / (Ns + 2)).
    const fs::path dir = writeModel("kat_mtl_no_pr", "newmtl painted\nKd 1 1 1\nNs 96.0\n", { "painted" });

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "model.obj").string()));

    const ObjMaterial &material = loader.getMaterials()[loader.getMaterialIndices()[0]];
    EXPECT_NEAR(material.roughness, std::sqrt(2.0F / 98.0F), 1e-6F);

    fs::remove_all(dir);
}

TEST(ObjParseUnit, ADissolveBelowOneBlendsItsMesh)
{
    // d below 1 is BLEND, as in the WebGPU renderer (owner, 2026-11-19); tinyobjloader's ObjLoader stayed opaque.
    const fs::path dir =
      writeModel("kat_mtl_blend", "newmtl glass\nKd 1 1 1\nd 0.4\nnewmtl solid\nKd 1 1 1\n", { "glass", "solid" });

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "model.obj").string()));

    ASSERT_EQ(loader.getMeshRanges().size(), 2U);
    for (const Kataglyphis::MeshRange &range : loader.getMeshRanges()) {
        const ObjMaterial &material = loader.getMaterials()[loader.getMaterialIndices()[range.triStart]];
        EXPECT_EQ(range.alphaBlended, material.dissolve < 1.0F);
        EXPECT_EQ(material.alphaMode == ALPHA_BLEND, material.dissolve < 1.0F);
    }

    fs::remove_all(dir);
}

TEST(ObjParseUnit, EachUsemtlRunIsOneMeshRange)
{
    // One range per usemtl run (owner 2026-11-24): dinosaurs.obj's three objects use two materials, `bone` twice.
    if (!fs::exists(dinosaurs())) { GTEST_SKIP() << "test model not present"; }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(dinosaurs()));

    const auto &ranges = loader.getMeshRanges();
    EXPECT_EQ(ranges.size(), 3U);

    // Re-basing by -vertexBase is valid only if the ranges tile the arrays and no index leaves its block.
    std::size_t vsum = 0;
    std::size_t isum = 0;
    std::size_t tsum = 0;
    for (const auto &r : ranges) {
        EXPECT_EQ(r.vertexBase, vsum) << "vertex blocks must be contiguous";
        EXPECT_EQ(r.indexStart, isum) << "index blocks must be contiguous";
        EXPECT_EQ(r.triStart, tsum) << "material-index blocks must be contiguous";
        EXPECT_GT(r.vertexCount, 0U);
        for (std::size_t i = 0; i < r.indexCount; ++i) {
            const unsigned int idx = loader.getIndices()[r.indexStart + i];
            EXPECT_GE(static_cast<std::size_t>(idx), r.vertexBase);
            EXPECT_LT(static_cast<std::size_t>(idx), r.vertexBase + r.vertexCount)
              << "an index escapes its own vertex block - the re-base would corrupt it";
        }
        for (std::size_t t = 0; t < r.triCount; ++t) {
            EXPECT_EQ(loader.getMaterialIndices()[r.triStart + t], loader.getMaterialIndices()[r.triStart])
              << "a range draws one material";
        }
        vsum += r.vertexCount;
        isum += r.indexCount;
        tsum += r.triCount;
    }
    EXPECT_EQ(vsum, loader.getVertices().size()) << "ranges must cover every vertex";
    EXPECT_EQ(isum, loader.getIndices().size()) << "ranges must cover every index";
    EXPECT_EQ(tsum, loader.getMaterialIndices().size()) << "ranges must cover every face material";
}

TEST(ObjParseUnit, SingleMaterialObjIsOneMeshRangeSpanningEverything)
{
    if (!fs::exists(test_model())) { GTEST_SKIP() << "test model not present"; }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(test_model()));

    const auto &ranges = loader.getMeshRanges();
    ASSERT_EQ(ranges.size(), 1U) << "shadow_rig.obj names no material -> one mesh";
    EXPECT_EQ(ranges[0].vertexCount, loader.getVertices().size());
    EXPECT_EQ(ranges[0].indexCount, loader.getIndices().size());
    EXPECT_EQ(ranges[0].triCount, loader.getMaterialIndices().size());
}

TEST(ModelPickerUnit, GltfModelsAppearInTheAvailableList)
{
    // The picker must offer glTF too, not only .obj.
    const auto paths = sceneConfig::getAvailableModelPaths();
    if (paths.empty()) { GTEST_SKIP() << "no Resources/Models directory in this environment"; }

    const bool has_gltf = std::any_of(paths.begin(), paths.end(), [](const std::string &path) {
        auto lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return lower.ends_with(".glb") || lower.ends_with(".gltf");
    });
    EXPECT_TRUE(has_gltf) << "no glTF asset in the model picker list - the extension filter is OBJ-only again";
}

TEST(ModelPickerUnit, AddingANullModelIsSafeNotACrash)
{
    // Loaders return nullptr for malformed assets the GUI can pick.
    Kataglyphis::Scene scene;
    scene.add_model(nullptr);
    EXPECT_EQ(scene.getModelCount(), 0U) << "a failed load must leave the scene unchanged";
}

TEST(ObjParseUnit, AMissingFileFailsInsteadOfKillingTheProcess)
{
    // The GUI picker hands this arbitrary files, so a bad one must fail the parse, not exit.
    Kataglyphis::GltfLoader loader;

    EXPECT_FALSE(loader.parseCpu("this/path/does/not/exist.obj"));
    EXPECT_TRUE(loader.getVertices().empty()) << "a failed parse must leave no partial geometry";
}

TEST(ObjParseUnit, ReparsingReplacesRatherThanAppends)
{
    if (!fs::exists(test_model())) { GTEST_SKIP() << "test model not present"; }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(test_model()));
    const size_t first_vertices = loader.getVertices().size();
    const size_t first_indices = loader.getIndices().size();

    ASSERT_TRUE(loader.parseCpu(test_model()));

    // Reload reuses a loader; accumulating doubles the mesh, which looks like z-fighting, not a leak.
    EXPECT_EQ(loader.getVertices().size(), first_vertices);
    EXPECT_EQ(loader.getIndices().size(), first_indices);
}

TEST(ObjParseUnit, ParsesOnAWorkerThreadWithTheSameResult)
{
    if (!fs::exists(test_model())) { GTEST_SKIP() << "test model not present"; }

    // The async loader needs a parse free of thread-local and device state.
    Kataglyphis::GltfLoader on_main;
    ASSERT_TRUE(on_main.parseCpu(test_model()));

    Kataglyphis::GltfLoader on_worker;
    bool worker_ok = false;
    std::thread worker([&] { worker_ok = on_worker.parseCpu(test_model()); });
    worker.join();

    ASSERT_TRUE(worker_ok);
    ASSERT_EQ(on_worker.getVertices().size(), on_main.getVertices().size());
    ASSERT_EQ(on_worker.getIndices().size(), on_main.getIndices().size());
    for (size_t i = 0; i < on_main.getIndices().size(); ++i) {
        ASSERT_EQ(on_worker.getIndices()[i], on_main.getIndices()[i]) << "index " << i << " differs off-thread";
    }
}

TEST(ObjParseUnit, AFaceWithAnOutOfRangeIndexIsDroppedWhole)
{
    // Dropping only the bad corner desyncs materialIndex from indices/3, so the whole face goes.
    const fs::path tmp = fs::temp_directory_path() / "kat_bad_face" / "model.obj";
    writeFile(tmp, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\nf 1 2 999999\n");

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));

    EXPECT_EQ(loader.getIndices().size(), 3U) << "the good face survives, the bad one is gone entirely";
    EXPECT_EQ(loader.getMaterialIndices().size(), loader.getIndices().size() / 3U);

    fs::remove_all(tmp.parent_path());
}

TEST(ObjParseUnit, AFileWithOnlyMalformedFacesFailsTheParse)
{
    // Nothing would draw, and an empty load must differ from a failed one.
    const fs::path tmp = fs::temp_directory_path() / "kat_only_bad_face" / "model.obj";
    writeFile(tmp, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 999999\n");

    Kataglyphis::GltfLoader loader;
    EXPECT_FALSE(loader.parseCpu(tmp.string()));
    EXPECT_TRUE(loader.getVertices().empty());

    fs::remove_all(tmp.parent_path());
}

TEST(ObjParseUnit, DegenerateTrianglesDoNotProduceNaNNormals)
{
    // A zero-area triangle's cross product would normalize to a NaN normal.
    const fs::path tmp = fs::temp_directory_path() / "kat_degenerate_face" / "model.obj";
    writeFile(tmp, "v 0 0 0\nv 0 0 0\nv 1 0 0\nf 1 2 3\n");

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));

    ASSERT_FALSE(loader.getVertices().empty());
    for (const auto &vertex : loader.getVertices()) {
        EXPECT_TRUE(std::isfinite(vertex.normal.x) && std::isfinite(vertex.normal.y) && std::isfinite(vertex.normal.z));
        EXPECT_TRUE(std::isfinite(vertex.tangent.x) && std::isfinite(vertex.tangent.w));
    }

    fs::remove_all(tmp.parent_path());
}

TEST(ObjParseUnit, FacesWithoutANormalIndexGetAFlatNormal)
{
    // Only the second face omits `vn`, so a whole-file check would leave it with zero normals.
    const fs::path tmp = fs::temp_directory_path() / "kat_mixed_normals" / "model.obj";
    writeFile(tmp,
      "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
      "v 0 0 1\nv 1 0 1\nv 0 1 1\n"
      "vn 0 0 1\n"
      "f 1//1 2//1 3//1\n"
      "f 4 5 6\n");

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));

    ASSERT_EQ(loader.getVertices().size(), 6U);
    for (const auto &vertex : loader.getVertices()) {
        EXPECT_NEAR(glm::dot(vertex.normal, vertex.normal), 1.0F, 1e-4F);
        EXPECT_NEAR(vertex.normal.z, 1.0F, 1e-4F) << "both triangles face +Z";
    }

    fs::remove_all(tmp.parent_path());
}

TEST(ObjParseUnit, MtlTexturesResolveBesideTheMtlUnderTexturesAndThroughABackslash)
{
    // Beside the .mtl is the format's rule; textures/ is every shipped OBJ's layout; `\` is a Windows export's.
    const fs::path dir = writeModel("kat_mtl_texture_paths",
      "newmtl beside\nmap_Kd paint.png\nnewmtl sub\nmap_Kd wood.png\nnewmtl windows\nmap_Kd textures\\stone.png\n",
      { "beside", "sub", "windows" },
      { { "paint.png", "paint" }, { "textures/wood.png", "wood" }, { "textures/stone.png", "stone" } });

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "model.obj").string()));

    ASSERT_EQ(loader.getMaterials().size(), 4U) << "three, then the neutral fallback";
    EXPECT_EQ(bytesOf(loader, loader.getMaterials()[0].textureID), "paint");
    EXPECT_EQ(bytesOf(loader, loader.getMaterials()[1].textureID), "wood");
    EXPECT_EQ(bytesOf(loader, loader.getMaterials()[2].textureID), "stone");

    fs::remove_all(dir);
}

TEST(ObjParseUnit, TexturesResolveBesideAnMtlInASubfolderAndNeverOutsideTheModelsFolder)
{
    // The .mtl's folder is the base (tinyobjloader's rule); a path out of the OBJ's folder is refused like a glTF's.
    const fs::path dir = fs::temp_directory_path() / "kat_mtl_subfolder";
    fs::remove_all(dir);
    writeFile(dir / "secret.png", psdTagged("secret"));
    writeFile(dir / "model" / "sub" / "x.mtl", "newmtl near\nmap_Kd near.png\nnewmtl out\nmap_Kd ../../secret.png\n");
    writeFile(dir / "model" / "sub" / "near.png", psdTagged("near"));
    writeFile(dir / "model" / "near.png", psdTagged("wrong"));
    writeFile(dir / "model" / "model.obj",
      "mtllib sub/x.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl near\nf 1 2 3\nusemtl out\nf 1 2 3\n");

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "model" / "model.obj").string()));

    ASSERT_EQ(bytesOf(loader, loader.getMaterials()[0].textureID), "near");
    EXPECT_EQ(loader.getMaterials()[1].textureID, -1) << "a texture outside the model's folder is never read";
    EXPECT_EQ(loader.getTextures().size(), 1U);

    fs::remove_all(dir);
}

TEST(ObjParseUnit, OneFileSharesASlotPerColourSpace)
{
    // A slot carries one image format: sRGB uses of one file share it, a linear use needs its own.
    const fs::path dir = writeModel("kat_mtl_shared_texture",
      "newmtl first\nmap_Kd wood.png\nmap_Ke wood.png\nKe 1 1 1\nmap_Bump wood.png\n"
      "newmtl second\nmap_Kd ./wood.png\n",
      { "first", "second" },
      { { "wood.png", "wood" } });

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "model.obj").string()));

    const ObjMaterial &first = loader.getMaterials()[0];
    const ObjMaterial &second = loader.getMaterials()[1];
    EXPECT_EQ(first.textureID, second.textureID) << "two spellings of one file share a slot";
    EXPECT_EQ(first.textureID, first.emissiveTextureID) << "map_Kd and map_Ke are both sRGB";
    EXPECT_NE(first.textureID, first.normalTextureID);
    ASSERT_EQ(loader.getTextures().size(), 2U);
    EXPECT_EQ(loader.getTextureSrgbFlags()[static_cast<std::size_t>(first.textureID)], 1);
    EXPECT_EQ(loader.getTextureSrgbFlags()[static_cast<std::size_t>(first.normalTextureID)], 0);

    fs::remove_all(dir);
}

TEST(ObjParseUnit, NormWinsOverMapBumpWithItsOwnBumpMultiplier)
{
    // norm is a tangent-space normal map, map_Bump conventionally a height map; the winner's -bm counts.
    const fs::path dir = writeModel("kat_mtl_norm_preferred",
      "newmtl leak\nmap_Bump -bm 3.0 height.png\nnorm rock_n.png\n"
      "newmtl scaled\nnorm -bm 0.5 rock_n.png\n"
      "newmtl bump\nmap_Bump height.png\n"
      "newmtl plain\nKd 1 1 1\n",
      { "leak", "scaled", "bump", "plain" },
      { { "height.png", "height" }, { "rock_n.png", "normal" } });

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "model.obj").string()));

    const auto &m = loader.getMaterials();
    EXPECT_EQ(bytesOf(loader, m[0].normalTextureID), "normal");
    EXPECT_FLOAT_EQ(m[0].normalScale, 1.0F) << "map_Bump's -bm must not leak onto the preferred norm";
    EXPECT_FLOAT_EQ(m[1].normalScale, 0.5F);
    EXPECT_EQ(bytesOf(loader, m[2].normalTextureID), "height") << "map_Bump alone is the normal map";
    EXPECT_EQ(m[3].normalTextureID, -1);

    fs::remove_all(dir);
}

TEST(ObjParseUnit, MapDBecomesAnAlphaTextureAndAMaskCutoff)
{
    // map_d is a linear cut-out mask at 0.5; the shaders sample it beside the base colour.
    const fs::path dir = writeModel("kat_mtl_map_d",
      "newmtl leafy\nKd 1 1 1\nmap_Kd base.png\nmap_d mask.png\nnewmtl solid\nKd 1 1 1\nmap_Kd base.png\n",
      { "leafy", "solid" },
      { { "base.png", "base" }, { "mask.png", "mask" } });

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "model.obj").string()));

    const ObjMaterial &leafy = loader.getMaterials()[0];
    ASSERT_EQ(bytesOf(loader, leafy.alphaTextureID), "mask");
    EXPECT_EQ(loader.getTextureSrgbFlags()[static_cast<std::size_t>(leafy.alphaTextureID)], 0);
    EXPECT_FLOAT_EQ(leafy.alphaCutoff, 0.5F);
    EXPECT_EQ(leafy.alphaMode, ALPHA_MASK);

    const ObjMaterial &solid = loader.getMaterials()[1];
    EXPECT_EQ(solid.alphaTextureID, -1) << "a material without map_d must keep the sentinel";
    EXPECT_EQ(solid.alphaMode, ALPHA_OPAQUE) << "a material without map_d must not become a MASK material";

    fs::remove_all(dir);
}

TEST(ObjParseUnit, AMissingTextureDrawsTheMaterialUntextured)
{
    const fs::path dir =
      writeModel("kat_mtl_missing_texture", "newmtl gone\nKd 1 0 0\nmap_Kd nowhere.png\n", { "gone" });

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "model.obj").string()));

    EXPECT_EQ(loader.getMaterials()[0].textureID, -1);
    EXPECT_TRUE(loader.getTextures().empty());

    fs::remove_all(dir);
}

TEST(ObjParseUnit, UploadParsedOnADeviceFreeLoaderReturnsNull)
{
    // The device check comes first, so a device-free loader reports the device.
    if (!fs::exists(test_model())) { GTEST_SKIP() << "test model not present"; }

    Kataglyphis::GltfLoader loader;// device-free constructor
    ASSERT_TRUE(loader.parseCpu(test_model()));

    EXPECT_EQ(loader.uploadParsed(), nullptr) << "uploadParsed must refuse to run without a device";
}
