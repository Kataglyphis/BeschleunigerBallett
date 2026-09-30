// ObjLoader{} is the CPU-only constructor, so these fail if parseCpu ever touches Vulkan.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>
#include <string>
#include <memory>
#include <thread>

import kataglyphis.vulkan.obj_loader;
import kataglyphis.vulkan.scene_config;
import kataglyphis.vulkan.scene;

namespace {

std::string test_model()
{
    // The purpose-built shadow rig: small, committed, and known-good.
    return sceneConfig::resolveModelPath("Models/ShadowTest/shadow_rig.obj");
}

}// namespace

TEST(ObjParseUnit, ParsesWithoutAVulkanDevice)
{
    if (!std::filesystem::exists(test_model())) { GTEST_SKIP() << "test model not present"; }

    Kataglyphis::ObjLoader loader;// no device, no queue, no command pool
    ASSERT_TRUE(loader.parseCpu(test_model()));

    EXPECT_GT(loader.getVertices().size(), 0U);
    EXPECT_GT(loader.getIndices().size(), 0U);
    EXPECT_EQ(loader.getIndices().size() % 3U, 0U) << "indices must form whole triangles";
}

TEST(ObjParseUnit, FacesWithoutAMaterialIndexInsideTheMaterialsArray)
{
    if (!std::filesystem::exists(test_model())) { GTEST_SKIP() << "test model not present"; }

    // Without a mtllib tinyobj reports material_id -1, which as uint32 makes every material fetch read OOB.
    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu(test_model()));

    ASSERT_FALSE(loader.getMaterials().empty())
      << "a file without materials must still yield the default material";
    for (const unsigned int index : loader.getMaterialIndices()) {
        ASSERT_LT(index, loader.getMaterials().size())
          << "face material index escapes the materials array (GPU OOB read)";
    }
}

TEST(ObjParseUnit, AnObjWithoutAnMtlGetsANonEmittingMaterial)
{
    // No authored Ke means no emitted radiance, or every .obj without an .mtl glows.
    const auto tmp = std::filesystem::temp_directory_path() / "kat_no_mtllib.obj";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    }

    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));

    ASSERT_EQ(loader.getMaterials().size(), 1U);
    const glm::vec3 &emission = loader.getMaterials().front().emission;
    EXPECT_FLOAT_EQ(emission.x, 0.0F);
    EXPECT_FLOAT_EQ(emission.y, 0.0F);
    EXPECT_FLOAT_EQ(emission.z, 0.0F);

    std::filesystem::remove(tmp);
}

TEST(ObjParseUnit, UntexturedMtlMaterialsRouteToTheDiffuseFallback)
{
    const std::string dino = sceneConfig::resolveModelPath("Models/Dinosaurs/dinosaurs.obj");
    if (!std::filesystem::exists(dino)) { GTEST_SKIP() << "test model not present"; }

    // The shaders' diffuse fallback keys on -1; slot 0 would sample the white default instead of Kd.
    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu(dino));

    ASSERT_FALSE(loader.getMaterials().empty());
    for (const auto &material : loader.getMaterials()) {
        EXPECT_EQ(material.get_textureID(), -1)
          << "a material without map_Kd must route to the diffuse fallback";
    }
}

TEST(ObjParseUnit, MaterialsHaveZeroMetallic)
{
    const std::string dino = sceneConfig::resolveModelPath("Models/Dinosaurs/dinosaurs.obj");
    if (!std::filesystem::exists(dino)) { GTEST_SKIP() << "test model not present"; }

    // dinosaurs.mtl authors no Pm, so metallic stays at ObjMaterial's default.
    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu(dino));

    ASSERT_FALSE(loader.getMaterials().empty());
    for (const auto &material : loader.getMaterials()) {
        EXPECT_NEAR(material.metallic, 0.0F, 1e-6F) << "a .mtl without Pm must default to metallic 0.0";
    }
}

TEST(ObjParseUnit, MtlPbrChannelsReachTheMaterial)
{
    const auto dir = std::filesystem::temp_directory_path() / "kat_mtl_pbr_channels";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    {
        std::ofstream mtl(dir / "pbr.mtl", std::ios::binary);
        mtl << "newmtl painted\nKd 1 1 1\nPm 1.0\nPr 0.25\n";
    }
    {
        std::ofstream obj(dir / "pbr.obj", std::ios::binary);
        obj << "mtllib pbr.mtl\n"
               "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
               "usemtl painted\nf 1 2 3\n";
    }

    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "pbr.obj").string()));

    ASSERT_EQ(loader.getMaterials().size(), 1U);
    EXPECT_FLOAT_EQ(loader.getMaterials()[0].metallic, 1.0F);
    EXPECT_FLOAT_EQ(loader.getMaterials()[0].roughness, 0.25F);

    std::filesystem::remove_all(dir);
}

TEST(ObjParseUnit, AnMtlWithoutPrKeepsTheShininessSentinel)
{
    // tinyobjloader's Pr default 0.0 is a perfect mirror, indistinguishable from "not authored".
    const auto dir = std::filesystem::temp_directory_path() / "kat_mtl_no_pr";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    {
        std::ofstream mtl(dir / "no_pr.mtl", std::ios::binary);
        mtl << "newmtl painted\nKd 1 1 1\nNs 96.0\n";
    }
    {
        std::ofstream obj(dir / "no_pr.obj", std::ios::binary);
        obj << "mtllib no_pr.mtl\n"
               "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
               "usemtl painted\nf 1 2 3\n";
    }

    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "no_pr.obj").string()));

    ASSERT_EQ(loader.getMaterials().size(), 1U);
    EXPECT_LT(loader.getMaterials()[0].roughness, 0.0F)
      << "an .mtl without Pr must keep the shininess-derived sentinel";

    std::filesystem::remove_all(dir);
}

TEST(ObjParseUnit, MultiShapeObjRecordsPerShapeMeshRanges)
{
    // Each `o`/`g` shape becomes its own MeshRange; dinosaurs.obj has three.
    const std::string dino = sceneConfig::resolveModelPath("Models/Dinosaurs/dinosaurs.obj");
    if (!std::filesystem::exists(dino)) { GTEST_SKIP() << "test model not present"; }

    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu(dino));

    const auto &ranges = loader.getMeshRanges();
    EXPECT_GT(ranges.size(), 1U) << "a multi-shape OBJ must split into more than one mesh";

    // Re-basing by -vertexBase is valid only if the ranges tile the arrays and no index leaves its block.
    std::size_t vsum = 0;
    std::size_t isum = 0;
    std::size_t tsum = 0;
    for (const auto &r : ranges) {
        EXPECT_EQ(r.vertexBase, vsum) << "vertex blocks must be contiguous";
        EXPECT_EQ(r.indexStart, isum) << "index blocks must be contiguous";
        EXPECT_EQ(r.triStart, tsum) << "material-index blocks must be contiguous";
        EXPECT_GT(r.vertexCount, 0U);
        EXPECT_EQ(r.indexCount % 3U, 0U) << "each shape's indices form whole triangles";
        for (std::size_t i = 0; i < r.indexCount; ++i) {
            const unsigned int idx = loader.getIndices()[r.indexStart + i];
            EXPECT_GE(static_cast<std::size_t>(idx), r.vertexBase);
            EXPECT_LT(static_cast<std::size_t>(idx), r.vertexBase + r.vertexCount)
              << "a shape index escapes its own vertex block - the slice re-base would corrupt it";
        }
        vsum += r.vertexCount;
        isum += r.indexCount;
        tsum += r.triCount;
    }
    EXPECT_EQ(vsum, loader.getVertices().size()) << "ranges must cover every vertex";
    EXPECT_EQ(isum, loader.getIndices().size()) << "ranges must cover every index";
    EXPECT_EQ(tsum, loader.getMaterialIndices().size()) << "ranges must cover every face material";
}

TEST(ObjParseUnit, SingleShapeObjIsOneMeshRangeSpanningEverything)
{
    // A single-object OBJ must still build one mesh.
    if (!std::filesystem::exists(test_model())) { GTEST_SKIP() << "test model not present"; }

    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu(test_model()));

    const auto &ranges = loader.getMeshRanges();
    ASSERT_EQ(ranges.size(), 1U) << "shadow_rig.obj is a single shape -> one mesh, as before";
    EXPECT_EQ(ranges[0].vertexBase, 0U);
    EXPECT_EQ(ranges[0].vertexCount, loader.getVertices().size());
    EXPECT_EQ(ranges[0].indexStart, 0U);
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

TEST(ObjParseUnit, MalformedInputFailsInsteadOfKillingTheProcess)
{
    // The GUI picker hands this arbitrary files, so a bad one must fail the parse, not exit.
    Kataglyphis::ObjLoader loader;

    EXPECT_FALSE(loader.parseCpu("this/path/does/not/exist.obj"));
    EXPECT_TRUE(loader.getVertices().empty()) << "a failed parse must leave no partial geometry";
}

TEST(ObjParseUnit, ReparsingReplacesRatherThanAppends)
{
    if (!std::filesystem::exists(test_model())) { GTEST_SKIP() << "test model not present"; }

    Kataglyphis::ObjLoader loader;
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
    if (!std::filesystem::exists(test_model())) { GTEST_SKIP() << "test model not present"; }

    // The async loader needs a parse free of thread-local and device state.
    Kataglyphis::ObjLoader on_main;
    ASSERT_TRUE(on_main.parseCpu(test_model()));

    Kataglyphis::ObjLoader on_worker;
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
    const auto tmp = std::filesystem::temp_directory_path() / "kat_bad_face.obj";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\nf 1 2 999999\n";
    }

    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));

    EXPECT_EQ(loader.getIndices().size() % 3U, 0U) << "indices must form whole triangles";
    EXPECT_EQ(loader.getIndices().size(), 3U) << "the good face survives, the bad one is gone entirely";
    EXPECT_EQ(loader.getMaterialIndices().size(), loader.getIndices().size() / 3U);

    std::filesystem::remove(tmp);
}

TEST(ObjParseUnit, AFileWithOnlyMalformedFacesYieldsNoGeometry)
{
    // Every face falls to the guard, leaving empty arrays.
    const auto tmp = std::filesystem::temp_directory_path() / "kat_only_bad_face.obj";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 999999\n";
    }

    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string())) << "a malformed face must not fail the parse itself";

    EXPECT_TRUE(loader.getIndices().empty()) << "a file with only malformed faces yields no geometry";
    EXPECT_TRUE(loader.getVertices().empty());
    EXPECT_TRUE(loader.getMaterialIndices().empty());

    std::filesystem::remove(tmp);
}

TEST(ObjParseUnit, DegenerateTrianglesDoNotProduceNaNNormals)
{
    // A zero-area triangle's cross product would normalize to a NaN normal.
    const auto tmp = std::filesystem::temp_directory_path() / "kat_degenerate_face.obj";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << "v 0 0 0\nv 0 0 0\nv 1 0 0\nf 1 2 3\n";
    }

    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));

    ASSERT_FALSE(loader.getVertices().empty());
    for (const auto &vertex : loader.getVertices()) {
        EXPECT_TRUE(std::isfinite(vertex.normal.x));
        EXPECT_TRUE(std::isfinite(vertex.normal.y));
        EXPECT_TRUE(std::isfinite(vertex.normal.z));
    }

    std::filesystem::remove(tmp);
}

TEST(ObjParseUnit, FacesWithoutANormalIndexGetAFlatNormal)
{
    // Only the second face omits `vn`, so a whole-file check would leave it with zero normals.
    const auto tmp = std::filesystem::temp_directory_path() / "kat_mixed_normals.obj";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
               "v 0 0 1\nv 1 0 1\nv 0 1 1\n"
               "vn 0 0 1\n"
               "f 1//1 2//1 3//1\n"
               "f 4 5 6\n";
    }

    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));

    ASSERT_FALSE(loader.getVertices().empty());
    for (const auto &vertex : loader.getVertices()) {
        const float len2 = glm::dot(vertex.normal, vertex.normal);
        EXPECT_NEAR(len2, 1.0F, 1e-4F) << "every vertex must end up with a unit-length normal";
    }

    std::filesystem::remove(tmp);
}

TEST(ObjParseUnit, FullyNormalLessObjStillGetsFlatNormals)
{
    const auto tmp = std::filesystem::temp_directory_path() / "kat_no_normals.obj";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    }

    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));

    ASSERT_FALSE(loader.getVertices().empty());
    for (const auto &vertex : loader.getVertices()) {
        const float len2 = glm::dot(vertex.normal, vertex.normal);
        EXPECT_NEAR(len2, 1.0F, 1e-4F);
        EXPECT_NEAR(vertex.normal.z, 1.0F, 1e-4F) << "flat normal for this triangle points along +Z";
    }

    std::filesystem::remove(tmp);
}

TEST(ObjParseUnit, MtlRelativeTextureIsResolvedBesideTheMtl)
{
    // The OBJ/MTL format resolves map_Kd relative to the .mtl's directory.
    const auto dir = std::filesystem::temp_directory_path() / "kat_mtl_beside";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    {
        std::ofstream mtl(dir / "beside.mtl", std::ios::binary);
        mtl << "newmtl painted\nKd 1 1 1\nmap_Kd paint.png\n";
    }
    { std::ofstream texture(dir / "paint.png", std::ios::binary); }
    {
        std::ofstream obj(dir / "beside.obj", std::ios::binary);
        obj << "mtllib beside.mtl\n"
               "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
               "vt 0 0\nvt 1 0\nvt 0 1\n"
               "usemtl painted\nf 1/1 2/2 3/3\n";
    }

    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "beside.obj").string()));

    ASSERT_FALSE(loader.getTextureNames().empty());
    EXPECT_TRUE(std::filesystem::exists(loader.getTextureNames()[0]))
      << "the resolved texture path must name a file that actually exists: "
      << loader.getTextureNames()[0];

    std::filesystem::remove_all(dir);
}

TEST(ObjParseUnit, MaterialsSharingAMapKdShareOneTextureSlot)
{
    const auto dir = std::filesystem::temp_directory_path() / "kat_mtl_shared_texture";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    {
        std::ofstream mtl(dir / "shared.mtl", std::ios::binary);
        mtl << "newmtl first\nKd 1 1 1\nmap_Kd paint.png\n"
               "newmtl second\nKd 1 1 1\nmap_Kd paint.png\n";
    }
    { std::ofstream texture(dir / "paint.png", std::ios::binary); }
    {
        std::ofstream obj(dir / "shared.obj", std::ios::binary);
        obj << "mtllib shared.mtl\n"
               "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
               "vt 0 0\nvt 1 0\nvt 0 1\n"
               "usemtl first\nf 1/1 2/2 3/3\n"
               "usemtl second\nf 1/1 2/2 4/3\n";
    }

    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "shared.obj").string()));

    ASSERT_EQ(loader.getMaterials().size(), 2U) << "both declared materials must be present";
    ASSERT_EQ(loader.getTextureNames().size(), 2U);
    EXPECT_FALSE(loader.getTextureNames()[0].empty()) << "the first material's texture must be recorded";
    EXPECT_TRUE(loader.getTextureNames()[1].empty()) << "the second, duplicate texture must not be re-pushed";
    EXPECT_EQ(loader.getMaterials()[0].textureID, 0);
    EXPECT_EQ(loader.getMaterials()[1].textureID, 0) << "the second material must share slot 0, not get its own";

    std::filesystem::remove_all(dir);
}

TEST(ObjParseUnit, MtlMapBumpBecomesTheNormalTextureSlot)
{
    const auto dir = std::filesystem::temp_directory_path() / "kat_mtl_map_bump";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    {
        std::ofstream mtl(dir / "bump.mtl", std::ios::binary);
        mtl << "newmtl painted\nKd 1 1 1\nmap_Kd wood.png\nmap_Bump wood_n.png\n";
    }
    { std::ofstream texture(dir / "wood.png", std::ios::binary); }
    { std::ofstream texture(dir / "wood_n.png", std::ios::binary); }
    {
        std::ofstream obj(dir / "bump.obj", std::ios::binary);
        obj << "mtllib bump.mtl\n"
               "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
               "vt 0 0\nvt 1 0\nvt 0 1\n"
               "usemtl painted\nf 1/1 2/2 3/3\n";
    }

    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "bump.obj").string()));

    ASSERT_EQ(loader.getMaterials().size(), 1U);
    const auto &material = loader.getMaterials()[0];
    EXPECT_GE(material.textureID, 0);
    EXPECT_GE(material.normalTextureID, 0);
    EXPECT_NE(material.textureID, material.normalTextureID)
      << "the diffuse and normal maps must not share a slot";

    std::filesystem::remove_all(dir);
}

TEST(ObjParseUnit, MtlNormPreferredOverMapBump)
{
    // norm is a true tangent-space normal map; map_Bump is conventionally a height map.
    const auto dir = std::filesystem::temp_directory_path() / "kat_mtl_norm_preferred";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    {
        std::ofstream mtl(dir / "norm.mtl", std::ios::binary);
        mtl << "newmtl painted\nKd 1 1 1\nmap_Bump height.png\nnorm normal.png\n";
    }
    { std::ofstream texture(dir / "height.png", std::ios::binary); }
    { std::ofstream texture(dir / "normal.png", std::ios::binary); }
    {
        std::ofstream obj(dir / "norm.obj", std::ios::binary);
        obj << "mtllib norm.mtl\n"
               "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
               "vt 0 0\nvt 1 0\nvt 0 1\n"
               "usemtl painted\nf 1/1 2/2 3/3\n";
    }

    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "norm.obj").string()));

    ASSERT_EQ(loader.getMaterials().size(), 1U);
    EXPECT_GE(loader.getMaterials()[0].normalTextureID, 0);

    // An absent map_Kd leaves a "" gap in getTextureNames(), so the ID is not a position there.
    bool sawNormalPng = false;
    for (const std::string &name : loader.getTextureNames()) {
        const auto filename = std::filesystem::path(name).filename();
        EXPECT_NE(filename, "height.png") << "map_Bump must be ignored when norm is also present";
        if (filename == "normal.png") { sawNormalPng = true; }
    }
    EXPECT_TRUE(sawNormalPng) << "norm must win over map_Bump when both are present";

    std::filesystem::remove_all(dir);
}

TEST(ObjParseUnit, AnMtlWithoutANormalMapKeepsTheMinusOneSentinel)
{
    const auto dir = std::filesystem::temp_directory_path() / "kat_mtl_no_normal";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    {
        std::ofstream mtl(dir / "flat.mtl", std::ios::binary);
        mtl << "newmtl painted\nKd 1 1 1\nmap_Kd wood.png\n";
    }
    { std::ofstream texture(dir / "wood.png", std::ios::binary); }
    {
        std::ofstream obj(dir / "flat.obj", std::ios::binary);
        obj << "mtllib flat.mtl\n"
               "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
               "vt 0 0\nvt 1 0\nvt 0 1\n"
               "usemtl painted\nf 1/1 2/2 3/3\n";
    }

    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "flat.obj").string()));

    ASSERT_EQ(loader.getMaterials().size(), 1U);
    EXPECT_EQ(loader.getMaterials()[0].normalTextureID, -1);

    std::filesystem::remove_all(dir);
}

TEST(ObjParseUnit, OneFileNamedAsBothMapKdAndMapBumpGetsTwoSlots)
{
    // A slot carries one image format, so sRGB and linear uses of one file need two.
    const auto dir = std::filesystem::temp_directory_path() / "kat_mtl_shared_file_two_slots";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    {
        std::ofstream mtl(dir / "dual.mtl", std::ios::binary);
        mtl << "newmtl painted\nKd 1 1 1\nmap_Kd wood.png\nmap_Bump wood.png\n";
    }
    { std::ofstream texture(dir / "wood.png", std::ios::binary); }
    {
        std::ofstream obj(dir / "dual.obj", std::ios::binary);
        obj << "mtllib dual.mtl\n"
               "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
               "vt 0 0\nvt 1 0\nvt 0 1\n"
               "usemtl painted\nf 1/1 2/2 3/3\n";
    }

    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "dual.obj").string()));

    ASSERT_EQ(loader.getMaterials().size(), 1U);
    const auto &material = loader.getMaterials()[0];
    EXPECT_GE(material.textureID, 0);
    EXPECT_GE(material.normalTextureID, 0);
    EXPECT_NE(material.textureID, material.normalTextureID)
      << "the same file used as sRGB base colour and linear normal map needs two slots";

    std::filesystem::remove_all(dir);
}

TEST(ObjParseUnit, MtlTextureUnderATexturesSubdirectoryIsResolved)
{
    // Every shipped OBJ uses this layout: a bare map_Kd filename with the file in textures/.
    const auto dir = std::filesystem::temp_directory_path() / "kat_mtl_textures_subdir";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "textures");

    {
        std::ofstream mtl(dir / "sub.mtl", std::ios::binary);
        mtl << "newmtl painted\nKd 1 1 1\nmap_Kd paint.png\n";
    }
    { std::ofstream texture(dir / "textures" / "paint.png", std::ios::binary); }
    {
        std::ofstream obj(dir / "sub.obj", std::ios::binary);
        obj << "mtllib sub.mtl\n"
               "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
               "vt 0 0\nvt 1 0\nvt 0 1\n"
               "usemtl painted\nf 1/1 2/2 3/3\n";
    }

    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "sub.obj").string()));

    ASSERT_FALSE(loader.getTextureNames().empty());
    EXPECT_TRUE(std::filesystem::exists(loader.getTextureNames()[0]))
      << "the textures/-subdirectory candidate must resolve: " << loader.getTextureNames()[0];

    std::filesystem::remove_all(dir);
}

TEST(ObjParseUnit, BackslashMapKdResolvesLikeAForwardSlash)
{
    // A Windows-authored backslash map_Kd must resolve like its forward-slash form.
    const std::string obj = sceneConfig::resolveModelPath("Models/VikingRoom/viking_room.obj");
    if (!std::filesystem::exists(obj)) { GTEST_SKIP() << "VikingRoom asset not present"; }
    const std::string dir = std::filesystem::path(obj).parent_path().string();

    const std::string backslash_result = Kataglyphis::resolveObjTexturePath(dir, "textures\\viking_room.png");
    const std::string forward_slash_result = Kataglyphis::resolveObjTexturePath(dir, "textures/viking_room.png");

    EXPECT_EQ(backslash_result, forward_slash_result);
    EXPECT_TRUE(std::filesystem::exists(backslash_result))
      << "the resolved path must name a file that actually exists: " << backslash_result;
}

TEST(ObjParseUnit, BareFilenameFallsBackToTheTexturesSubdirectory)
{
    const std::string obj = sceneConfig::resolveModelPath("Models/VikingRoom/viking_room.obj");
    if (!std::filesystem::exists(obj)) { GTEST_SKIP() << "VikingRoom asset not present"; }
    const std::string dir = std::filesystem::path(obj).parent_path().string();

    const std::string resolved = Kataglyphis::resolveObjTexturePath(dir, "viking_room.png");

    EXPECT_TRUE(std::filesystem::exists(resolved))
      << "a bare filename must fall back to the textures/ subdirectory: " << resolved;
}

TEST(ObjParseUnit, EmptyBaseDirStaysRelative)
{
    // An empty base dir must not become a filesystem-root path.
    const std::string resolved = Kataglyphis::resolveObjTexturePath("", "viking_room.png");

    EXPECT_FALSE(resolved.starts_with('/')) << "an empty base dir must not resolve against the filesystem root: "
                                             << resolved;
    EXPECT_FALSE(std::filesystem::path(resolved).is_absolute());
}

TEST(ObjParseUnit, NormDirectiveBumpMultiplierReachesNormalScale)
{
    // bump_texopt holds tinyobjloader's default 1.0 without map_Bump, so norm's own -bm must be read.
    const auto dir = std::filesystem::temp_directory_path() / "kat_mtl_norm_bump_multiplier";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    {
        std::ofstream mtl(dir / "norm_bm.mtl", std::ios::binary);
        mtl << "newmtl painted\nKd 1 1 1\nnorm -bm 0.5 rock_n.png\n";
    }
    { std::ofstream texture(dir / "rock_n.png", std::ios::binary); }
    {
        std::ofstream obj(dir / "norm_bm.obj", std::ios::binary);
        obj << "mtllib norm_bm.mtl\n"
               "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
               "vt 0 0\nvt 1 0\nvt 0 1\n"
               "usemtl painted\nf 1/1 2/2 3/3\n";
    }

    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "norm_bm.obj").string()));

    ASSERT_EQ(loader.getMaterials().size(), 1U);
    EXPECT_FLOAT_EQ(loader.getMaterials()[0].normalScale, 0.5F)
      << "norm's own -bm must reach normalScale even with no map_Bump present";

    std::filesystem::remove_all(dir);
}

TEST(ObjParseUnit, BumpMultiplierDoesNotLeakFromMapBumpOntoAPreferredNormDirective)
{
    // norm wins the slot, so its -bm must win too.
    const auto dir = std::filesystem::temp_directory_path() / "kat_mtl_bump_multiplier_no_leak";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    {
        std::ofstream mtl(dir / "no_leak.mtl", std::ios::binary);
        mtl << "newmtl painted\nKd 1 1 1\nmap_Bump -bm 3.0 height.png\nnorm rock_n.png\n";
    }
    { std::ofstream texture(dir / "height.png", std::ios::binary); }
    { std::ofstream texture(dir / "rock_n.png", std::ios::binary); }
    {
        std::ofstream obj(dir / "no_leak.obj", std::ios::binary);
        obj << "mtllib no_leak.mtl\n"
               "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
               "vt 0 0\nvt 1 0\nvt 0 1\n"
               "usemtl painted\nf 1/1 2/2 3/3\n";
    }

    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "no_leak.obj").string()));

    ASSERT_EQ(loader.getMaterials().size(), 1U);
    const auto &material = loader.getMaterials()[0];
    EXPECT_FLOAT_EQ(material.normalScale, 1.0F)
      << "map_Bump's -bm must not leak onto the preferred norm directive";

    bool sawRockN = false;
    for (const std::string &name : loader.getTextureNames()) {
        if (std::filesystem::path(name).filename() == "rock_n.png") { sawRockN = true; }
    }
    EXPECT_TRUE(sawRockN) << "norm must still win the texture slot";

    std::filesystem::remove_all(dir);
}

TEST(ObjParseUnit, MapKeBecomesTheEmissiveTextureSlot)
{
    // map_Ke is authored colour like map_Kd, so it is sRGB.
    const auto dir = std::filesystem::temp_directory_path() / "kat_mtl_map_ke";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    {
        std::ofstream mtl(dir / "glow.mtl", std::ios::binary);
        mtl << "newmtl painted\nKd 1 1 1\nKe 1 1 1\nmap_Kd base.png\nmap_Ke glow.png\n";
    }
    { std::ofstream texture(dir / "base.png", std::ios::binary); }
    { std::ofstream texture(dir / "glow.png", std::ios::binary); }
    {
        std::ofstream obj(dir / "glow.obj", std::ios::binary);
        obj << "mtllib glow.mtl\n"
               "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
               "vt 0 0\nvt 1 0\nvt 0 1\n"
               "usemtl painted\nf 1/1 2/2 3/3\n";
    }

    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "glow.obj").string()));

    ASSERT_EQ(loader.getMaterials().size(), 1U);
    const auto &material = loader.getMaterials()[0];
    EXPECT_GE(material.emissiveTextureID, 0);
    EXPECT_NE(material.textureID, material.emissiveTextureID)
      << "the diffuse and emissive maps must not share a slot";

    ASSERT_EQ(loader.getTextureSrgbFlags().size(), loader.getTextureNames().size());
    bool sawGlowPngSrgb = false;
    for (size_t i = 0; i < loader.getTextureNames().size(); i++) {
        if (std::filesystem::path(loader.getTextureNames()[i]).filename() == "glow.png") {
            EXPECT_EQ(loader.getTextureSrgbFlags()[i], 1) << "map_Ke is authored colour and must be sRGB";
            sawGlowPngSrgb = true;
        }
    }
    EXPECT_TRUE(sawGlowPngSrgb) << "glow.png must have been pushed as a texture slot";

    std::filesystem::remove_all(dir);
}

TEST(ObjParseUnit, MapKdAndMapKeNamingOneFileShareASlot)
{
    // Both sRGB, so the (resolved path, srgb) key collapses them onto one slot.
    const auto dir = std::filesystem::temp_directory_path() / "kat_mtl_shared_file_ke_slot";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    {
        std::ofstream mtl(dir / "dual_ke.mtl", std::ios::binary);
        mtl << "newmtl painted\nKd 1 1 1\nKe 1 1 1\nmap_Kd wood.png\nmap_Ke wood.png\n";
    }
    { std::ofstream texture(dir / "wood.png", std::ios::binary); }
    {
        std::ofstream obj(dir / "dual_ke.obj", std::ios::binary);
        obj << "mtllib dual_ke.mtl\n"
               "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
               "vt 0 0\nvt 1 0\nvt 0 1\n"
               "usemtl painted\nf 1/1 2/2 3/3\n";
    }

    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "dual_ke.obj").string()));

    ASSERT_EQ(loader.getMaterials().size(), 1U);
    const auto &material = loader.getMaterials()[0];
    EXPECT_GE(material.textureID, 0);
    EXPECT_EQ(material.textureID, material.emissiveTextureID)
      << "one file named as both map_Kd and map_Ke (both sRGB) must share a slot";

    std::filesystem::remove_all(dir);
}

TEST(ObjParseUnit, AMaterialWithoutMapKeKeepsTheSentinel)
{
    // No map_Ke means no slot pushed: one diffuse slot per material, nothing more.
    const std::string dino = sceneConfig::resolveModelPath("Models/Dinosaurs/dinosaurs.obj");
    if (!std::filesystem::exists(dino)) { GTEST_SKIP() << "test model not present"; }

    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu(dino));

    ASSERT_FALSE(loader.getMaterials().empty());
    for (const auto &material : loader.getMaterials()) {
        EXPECT_EQ(material.emissiveTextureID, -1) << "a material without map_Ke must keep the sentinel";
    }
    EXPECT_EQ(loader.getTextureNames().size(), loader.getMaterials().size())
      << "dinosaurs.mtl carries no map_Ke or normal/bump directives, so each material must push exactly "
         "one (possibly empty) diffuse slot and nothing more";
}

TEST(ObjParseUnit, MapDBecomesAnAlphaTextureAndAMaskCutoff)
{
    // There is no sorted transparent pass, so map_d becomes a MASK cut-out at 0.5.
    const auto dir = std::filesystem::temp_directory_path() / "kat_mtl_map_d";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    {
        std::ofstream mtl(dir / "cutout.mtl", std::ios::binary);
        mtl << "newmtl leafy\nKd 1 1 1\nmap_Kd base.png\nmap_d mask.png\n"
               "newmtl solid\nKd 1 1 1\nmap_Kd base.png\n";
    }
    { std::ofstream texture(dir / "base.png", std::ios::binary); }
    { std::ofstream texture(dir / "mask.png", std::ios::binary); }
    {
        std::ofstream obj(dir / "cutout.obj", std::ios::binary);
        obj << "mtllib cutout.mtl\n"
               "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
               "vt 0 0\nvt 1 0\nvt 0 1\n"
               "usemtl leafy\nf 1/1 2/2 3/3\n"
               "usemtl solid\nf 1/1 2/2 4/3\n";
    }

    Kataglyphis::ObjLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "cutout.obj").string()));

    ASSERT_EQ(loader.getMaterials().size(), 2U);
    const auto &leafy = loader.getMaterials()[0];
    EXPECT_GE(leafy.alphaTextureID, 0);
    EXPECT_NE(leafy.textureID, leafy.alphaTextureID)
      << "the diffuse and alpha maps must not share a slot";
    EXPECT_FLOAT_EQ(leafy.alphaCutoff, 0.5F);

    const auto &solid = loader.getMaterials()[1];
    EXPECT_EQ(solid.alphaTextureID, -1) << "a material without map_d must keep the sentinel";
    EXPECT_FLOAT_EQ(solid.alphaCutoff, -1.0F) << "a material without map_d must not become a MASK material";

    std::filesystem::remove_all(dir);
}

TEST(ObjParseUnit, UploadParsedOnADeviceFreeLoaderReturnsNull)
{
    // The device check comes first, so a device-free loader that never parsed reports the device.
    if (!std::filesystem::exists(test_model())) { GTEST_SKIP() << "test model not present"; }

    Kataglyphis::ObjLoader loader;// device-free constructor
    ASSERT_TRUE(loader.parseCpu(test_model()));

    EXPECT_EQ(loader.uploadParsed(), nullptr) << "uploadParsed must refuse to run without a device";
}

TEST(ObjParseUnit, UploadParsedBeforeAnyParseReturnsNull)
{
    Kataglyphis::ObjLoader loader;// device-free, and parseCpu never called

    EXPECT_EQ(loader.uploadParsed(), nullptr) << "uploadParsed must refuse to run before a successful parseCpu";
}
