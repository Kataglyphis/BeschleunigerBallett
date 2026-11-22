// OxidANT's fixtures, which both renderers load: Rust tests the parser, this the mapping into the engine's types.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <glm/glm.hpp>

#include "RepoFiles.hpp"
#include "ScopedEnvVar.hpp"
#include "oxidant_bridge/gltf.h"

import kataglyphis.shared.util.utf8_path;
import kataglyphis.vulkan.gltf_loader;
import kataglyphis.vulkan.mesh_range;
import kataglyphis.vulkan.obj_material;
import kataglyphis.vulkan.vertex;

namespace {

namespace ox = kataglyphis::oxidant::gltf;
namespace fs = std::filesystem;

using Kataglyphis::GltfLoader;
using Kataglyphis::GltfLoadOptions;
using Kataglyphis::TestSupport::oxidantGltfFixture;
using Kataglyphis::TestSupport::oxidantGltfFixtureDir;

// Every .gltf and .glb in the folder, so a fixture OxidANT adds is checked on its arrival.
std::vector<std::string> oxidantGltfFixtures()
{
    std::vector<std::string> names;
    std::error_code error;
    for (fs::directory_iterator it(oxidantGltfFixtureDir(), error), end; !error && it != end; it.increment(error)) {
        const fs::path extension = it->path().extension();
        if (it->is_regular_file(error) && (extension == ".gltf" || extension == ".glb")) {
            names.push_back(it->path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

ox::GltfModel bridgeLoad(const std::string &path)
{
    const std::vector<std::uint8_t> bytes = Kataglyphis::Shared::utf8PathBytes(fs::path(path));
    return ox::load_gltf_flat(rust::Slice<const std::uint8_t>(bytes.data(), bytes.size()));
}

glm::vec3 vec3(const std::array<float, 3> &value) { return { value[0], value[1], value[2] }; }

bool isIdentityRow(const glm::vec3 &row0, const glm::vec3 &row1)
{ return row0 == glm::vec3(1.0F, 0.0F, 0.0F) && row1 == glm::vec3(0.0F, 1.0F, 0.0F); }

void expectVerticesAsBridged(std::span<const Vertex> vertices, const ox::GltfModel &bridged)
{
    ASSERT_EQ(vertices.size(), bridged.vertices.size());
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const ox::GltfVertex &source = bridged.vertices[i];
        const Vertex &vertex = vertices[i];
        EXPECT_EQ(vertex.position, vec3(source.position)) << "vertex " << i;
        EXPECT_EQ(vertex.normal, vec3(source.normal)) << "vertex " << i;
        EXPECT_EQ(vertex.color, glm::vec4(source.color[0], source.color[1], source.color[2], source.color[3]));
        EXPECT_EQ(vertex.texture_coords, glm::vec2(source.uv0[0], source.uv0[1])) << "vertex " << i;
        EXPECT_EQ(
          vertex.tangent, glm::vec4(source.tangent[0], source.tangent[1], source.tangent[2], source.tangent[3]));
        EXPECT_EQ(vertex.texture_coords1, glm::vec2(source.uv1[0], source.uv1[1])) << "vertex " << i;
    }
}

void expectMaterialsAsBridged(const std::vector<ObjMaterial> &materials, const ox::GltfModel &bridged)
{
    ASSERT_EQ(materials.size(), bridged.materials.size());
    for (std::size_t i = 0; i < materials.size(); ++i) {
        const ox::GltfMaterial &source = bridged.materials[i];
        const ObjMaterial &material = materials[i];
        EXPECT_EQ(material.diffuse, vec3(source.diffuse)) << "material " << i;
        EXPECT_EQ(material.emission, vec3(source.emission)) << "material " << i;
        EXPECT_EQ(material.dissolve, source.dissolve) << "material " << i;
        EXPECT_EQ(material.alphaCutoff, source.alpha_cutoff) << "material " << i;
        EXPECT_EQ(material.metallic, source.metallic) << "material " << i;
        EXPECT_EQ(material.roughness, source.roughness) << "material " << i;
        EXPECT_EQ(material.normalScale, source.normal_scale) << "material " << i;
        EXPECT_EQ(material.textureID, source.base_texture) << "material " << i;
        EXPECT_EQ(material.metallicRoughnessTextureID, source.metallic_roughness_texture) << "material " << i;
        EXPECT_EQ(material.normalTextureID, source.normal_texture) << "material " << i;
        EXPECT_EQ(material.emissiveTextureID, source.emissive_texture) << "material " << i;
        EXPECT_EQ(material.uv_transform_row0, vec3(source.base_uv_row0)) << "material " << i;
        EXPECT_EQ(material.uv_transform_row1, vec3(source.base_uv_row1)) << "material " << i;
        EXPECT_EQ(material.metallic_roughness_uv_transform_row0, vec3(source.metallic_roughness_uv_row0));
        EXPECT_EQ(material.metallic_roughness_uv_transform_row1, vec3(source.metallic_roughness_uv_row1));
        EXPECT_EQ(material.normal_uv_transform_row0, vec3(source.normal_uv_row0)) << "material " << i;
        EXPECT_EQ(material.normal_uv_transform_row1, vec3(source.normal_uv_row1)) << "material " << i;
        EXPECT_EQ(material.emissive_uv_transform_row0, vec3(source.emissive_uv_row0)) << "material " << i;
        EXPECT_EQ(material.emissive_uv_transform_row1, vec3(source.emissive_uv_row1)) << "material " << i;
        EXPECT_EQ(material.unlit, source.unlit ? 1 : 0) << "material " << i;
        EXPECT_EQ(material.alphaBlend, source.alpha_blend ? 1 : 0) << "material " << i;
        EXPECT_EQ(material.uv1SlotMask, static_cast<int>(source.uv1_slot_mask)) << "material " << i;
        const int textures = static_cast<int>(bridged.textures.size());
        for (const int id : { material.textureID,
               material.metallicRoughnessTextureID,
               material.normalTextureID,
               material.emissiveTextureID }) {
            EXPECT_TRUE(id >= -1 && id < textures) << "material " << i << " names texture " << id;
        }
    }
}

// The upload re-bases each range's indices to its first vertex (Mesh's staging); every one must land in the range.
void expectRangesTileTheArrays(const GltfLoader &loader, const ox::GltfModel &bridged)
{
    const std::vector<Kataglyphis::MeshRange> &ranges = loader.getMeshRanges();
    ASSERT_EQ(ranges.size(), bridged.mesh_ranges.size());
    ASSERT_FALSE(ranges.empty()) << "a parsed model with no mesh range uploads nothing";
    const std::span<const unsigned int> indices = loader.getIndices();
    const std::span<const unsigned int> materialIndex = loader.getMaterialIndices();
    ASSERT_EQ(materialIndex.size() * 3, indices.size()) << "one material index per triangle";
    std::size_t nextIndex = 0;
    for (std::size_t r = 0; r < ranges.size(); ++r) {
        const Kataglyphis::MeshRange &range = ranges[r];
        const ox::GltfMeshRange &source = bridged.mesh_ranges[r];
        EXPECT_EQ(range.vertexBase, source.vertex_base) << "range " << r;
        EXPECT_EQ(range.vertexCount, source.vertex_count) << "range " << r;
        EXPECT_EQ(range.indexStart, source.index_start) << "range " << r;
        EXPECT_EQ(range.indexCount, source.index_count) << "range " << r;
        EXPECT_EQ(range.triStart, source.tri_start) << "range " << r;
        EXPECT_EQ(range.triCount, source.tri_count) << "range " << r;
        EXPECT_EQ(range.doubleSided, source.double_sided) << "range " << r;
        EXPECT_EQ(range.alphaBlended, source.alpha_blended) << "range " << r;
        ASSERT_TRUE(
          Kataglyphis::meshRangeFits(range, loader.getVertices().size(), indices.size(), materialIndex.size()))
          << "range " << r;
        EXPECT_EQ(range.indexStart, nextIndex) << "range " << r << " leaves a gap or overlaps the one before";
        EXPECT_EQ(range.triStart * 3, range.indexStart) << "range " << r;
        EXPECT_EQ(range.triCount * 3, range.indexCount) << "range " << r;
        nextIndex = range.indexStart + range.indexCount;

        std::vector<unsigned int> rebased(range.indexCount);
        Kataglyphis::rebaseIndices(
          indices.subspan(range.indexStart, range.indexCount), static_cast<unsigned int>(range.vertexBase), rebased);
        const bool inRange = std::all_of(
          rebased.begin(), rebased.end(), [&range](unsigned int index) { return index < range.vertexCount; });
        EXPECT_TRUE(inRange) << "range " << r << " names a vertex outside its own after the re-base";
        for (const unsigned int material : materialIndex.subspan(range.triStart, range.triCount)) {
            EXPECT_LT(material, loader.getMaterials().size()) << "range " << r;
        }
    }
    EXPECT_EQ(nextIndex, indices.size()) << "the ranges leave indices no mesh uploads";
}

// What a fixture of each kind must carry through: the categories the plan names, so a mapping drop shows by name.
const std::map<std::string, std::function<void(const GltfLoader &)>> &fixtureFacts()
{
    static const std::map<std::string, std::function<void(const GltfLoader &)>> facts{
        { "cube_alpha.gltf",
          [](const GltfLoader &loader) {
              const auto &materials = loader.getMaterials();
              EXPECT_TRUE(
                std::any_of(materials.begin(), materials.end(), [](const ObjMaterial &m) { return m.alphaBlend == 1; }))
                << "BLEND reached no material";
              EXPECT_TRUE(std::any_of(
                materials.begin(), materials.end(), [](const ObjMaterial &m) { return m.alphaCutoff >= 0.0F; }))
                << "MASK reached no material";
              const auto &ranges = loader.getMeshRanges();
              EXPECT_TRUE(std::any_of(ranges.begin(),
                ranges.end(),
                [](const Kataglyphis::MeshRange &range) { return range.alphaBlended && range.doubleSided; }))
                << "the BLEND primitive's range is not blended and double-sided";
          } },
        { "cube_basisu.gltf",
          [](const GltfLoader &loader) {
              EXPECT_TRUE(loader.getTextureImages().empty()) << "a KTX2-only image has no encoded slot";
              EXPECT_EQ(loader.getMaterials().at(0).textureID, -1);
          } },
        { "cube_basisu_fallback.gltf",
          [](const GltfLoader &loader) {
              ASSERT_EQ(loader.getTextureImages().size(), 1U) << "the PNG fallback is the slot";
              EXPECT_EQ(loader.getMaterials().at(0).textureID, 0);
          } },
        { "cube_normal_transform.gltf",
          [](const GltfLoader &loader) {
              const ObjMaterial &material = loader.getMaterials().at(0);
              EXPECT_GE(material.normalTextureID, 0);
              EXPECT_FALSE(isIdentityRow(material.normal_uv_transform_row0, material.normal_uv_transform_row1))
                << "the normal texture's KHR_texture_transform is lost";
          } },
        { "cube_on_plane.gltf", [](const GltfLoader &loader) { EXPECT_EQ(loader.getMeshRanges().size(), 2U); } },
        { "cube_quantized.gltf",
          [](const GltfLoader &loader) {
              for (const Vertex &vertex : loader.getVertices()) {
                  EXPECT_LE(glm::length(vertex.position), 0.867F)
                    << "the node's scale and translation did not dequantize";
              }
          } },
        { "cube_textured.gltf",
          [](const GltfLoader &loader) {
              ASSERT_EQ(loader.getTextureImages().size(), 1U);
              const ObjMaterial &material = loader.getMaterials().at(0);
              EXPECT_EQ(material.textureID, 0);
              EXPECT_FALSE(isIdentityRow(material.uv_transform_row0, material.uv_transform_row1))
                << "the base colour's KHR_texture_transform is lost";
          } },
        { "cube_unlit.gltf", [](const GltfLoader &loader) { EXPECT_EQ(loader.getMaterials().at(0).unlit, 1); } },
        { "point_light.gltf", [](const GltfLoader &loader) { EXPECT_EQ(loader.getPunctualLights().size(), 2U); } },
        { "skinned_bar.gltf", [](const GltfLoader &loader) { EXPECT_TRUE(loader.getMeshRanges().at(0).doubleSided); } },
        { "skinned_bar_quantized.gltf",
          [](const GltfLoader &loader) {
              for (const Vertex &vertex : loader.getVertices()) {
                  EXPECT_LE(vertex.position.y, 2.001F) << "the joints' inverse binds did not dequantize";
              }
          } },
    };
    return facts;
}

class GltfFixtureRoundTrip : public ::testing::TestWithParam<std::string>
{
};

std::string fixtureTestName(const ::testing::TestParamInfo<std::string> &info)
{
    std::string name = info.param;
    std::replace_if(name.begin(), name.end(), [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); }, '_');
    return name;
}

}// namespace

TEST_P(GltfFixtureRoundTrip, ReachesVertexMeshRangeAndObjMaterialAsTheBridgeReturnsIt)
{
    const std::string path = oxidantGltfFixture(GetParam());
    const ox::GltfModel bridged = bridgeLoad(path);
    ASSERT_TRUE(bridged.ok) << GetParam() << ": " << std::string(bridged.error);

    GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(path)) << GetParam();

    expectVerticesAsBridged(loader.getVertices(), bridged);
    EXPECT_TRUE(std::equal(
      loader.getIndices().begin(), loader.getIndices().end(), bridged.indices.begin(), bridged.indices.end()));
    EXPECT_TRUE(std::equal(loader.getMaterialIndices().begin(),
      loader.getMaterialIndices().end(),
      bridged.material_index.begin(),
      bridged.material_index.end()));
    expectMaterialsAsBridged(loader.getMaterials(), bridged);
    expectRangesTileTheArrays(loader, bridged);

    ASSERT_EQ(loader.getTextureImages().size(), bridged.textures.size());
    for (std::size_t t = 0; t < bridged.textures.size(); ++t) {
        const std::span<const unsigned char> encoded = loader.getTextureImages()[t];
        EXPECT_TRUE(std::equal(
          encoded.begin(), encoded.end(), bridged.textures[t].encoded.begin(), bridged.textures[t].encoded.end()))
          << "texture " << t;
        EXPECT_EQ(loader.getTextureSrgbFlags()[t] != 0, bridged.textures[t].srgb) << "texture " << t;
        EXPECT_EQ(loader.getTextureSamplerDescs()[t].mipmaps, bridged.textures[t].sampler.mipmaps) << "texture " << t;
    }
    EXPECT_EQ(loader.getPunctualLights().size(), bridged.lights.size());
    EXPECT_EQ(loader.getSun().has_value(), bridged.has_sun);

    if (const auto fact = fixtureFacts().find(GetParam()); fact != fixtureFacts().end()) { fact->second(loader); }
}

INSTANTIATE_TEST_SUITE_P(OxidantFixtures,
  GltfFixtureRoundTrip,
  ::testing::ValuesIn(oxidantGltfFixtures()),
  fixtureTestName);
// An empty scan is a failure below, not a silently uninstantiated suite.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(GltfFixtureRoundTrip);

// The plan's categories (morphs, skins, Basis, cameras, unlit, alpha) and every fixture with a fact must be scanned.
TEST(GltfParseUnit, OxidantsFixtureSetHoldsEveryKindTheRoundTripChecks)
{
    const std::vector<std::string> scanned = oxidantGltfFixtures();
    ASSERT_GE(scanned.size(), 10U) << "found only " << scanned.size() << " glTF fixture(s) under "
                                   << oxidantGltfFixtureDir().string() << " - is OxidANT checked out and staged?";
    std::vector<std::string> expected{
        "cube_morph.gltf", "skinned_bar.gltf", "cube_basisu.gltf", "cube_ortho_camera.gltf", "cube_animated.gltf"
    };
    for (const auto &[name, fact] : fixtureFacts()) { expected.push_back(name); }
    for (const std::string &name : expected) {
        EXPECT_NE(std::find(scanned.begin(), scanned.end(), name), scanned.end()) << name << " is not in the scan";
    }
}

// The mesh weight 1.0 lifts the cube's every vertex by 1; the engine draws no morphs, so the bridge bakes the pose.
TEST(GltfParseUnit, DefaultMorphWeightsArriveBakedIntoThePositions)
{
    GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(oxidantGltfFixture("cube_morph.gltf")));
    const std::span<const Vertex> vertices = loader.getVertices();
    ASSERT_EQ(vertices.size(), 24U);
    const auto by_height = [](const Vertex &a, const Vertex &b) { return a.position.y < b.position.y; };
    const auto [lowest, highest] = std::minmax_element(vertices.begin(), vertices.end(), by_height);
    EXPECT_FLOAT_EQ(lowest->position.y, 0.5F) << "the neutral cube's floor at -0.5 was not lifted";
    EXPECT_FLOAT_EQ(highest->position.y, 1.5F) << "the neutral cube's top at 0.5 was not lifted";
    for (const Vertex &vertex : vertices) {
        EXPECT_FLOAT_EQ(std::abs(vertex.position.x), 0.5F) << "the target moves along y alone";
    }
}

// gltfpack's layouts: the cube dequantized by its node, the skin by its joints at rest, which the bridge bakes.
TEST(GltfParseUnit, QuantizedFixturesArriveAsTheirFloatTwins)
{
    const std::array<std::array<const char *, 2>, 2> twins{ { { "cube.gltf", "cube_quantized.gltf" },
      { "skinned_bar.gltf", "skinned_bar_quantized.gltf" } } };
    for (const auto &[floatName, quantizedName] : twins) {
        GltfLoader floats;
        GltfLoader quantized;
        ASSERT_TRUE(floats.parseCpu(oxidantGltfFixture(floatName))) << floatName;
        ASSERT_TRUE(quantized.parseCpu(oxidantGltfFixture(quantizedName))) << quantizedName;
        const std::span<const Vertex> expected = floats.getVertices();
        const std::span<const Vertex> got = quantized.getVertices();
        ASSERT_EQ(got.size(), expected.size()) << quantizedName;
        for (std::size_t i = 0; i < got.size(); ++i) {
            // Half a 14-bit step of the bar's extent per axis, and an 8-bit normal's step.
            EXPECT_LE(glm::length(got[i].position - expected[i].position), 2e-4F) << quantizedName << " vertex " << i;
            EXPECT_LE(glm::length(got[i].normal - expected[i].normal), 1.5e-2F) << quantizedName << " vertex " << i;
        }
    }
}

// OxidANT's loader reads no environment since X14; MikkTSpace is asked for through the options alone.
TEST(GltfParseUnit, MikkTSpaceTangentsAreAskedForThroughTheLoadOptions)
{
    const std::string quad = oxidantGltfFixture("mirrored_uv_quad.gltf");
    GltfLoader lengyel;
    ASSERT_TRUE(lengyel.parseCpu(quad));
    EXPECT_EQ(lengyel.getVertices().size(), 4U) << "the default never splits a vertex";

    GltfLoader mikktspace;
    ASSERT_TRUE(mikktspace.parseCpu(quad, GltfLoadOptions{ .mikktspaceTangents = true }));
    EXPECT_EQ(mikktspace.getVertices().size(), 6U) << "MikkTSpace splits the two corners both UV charts share";
}

// The variable reaches a load only through gltfLoadOptionsFromEnvironment(), which App::run calls at the start.
TEST(GltfParseUnit, TheMikkTSpaceVariableReachesALoadOnlyThroughTheEngineStart)
{
    const std::string quad = oxidantGltfFixture("mirrored_uv_quad.gltf");
    {
        const Kataglyphis::TestSupport::ScopedEnvVar unset("KATAGLYPHIS_MIKKTSPACE_TANGENTS", "");
        EXPECT_FALSE(Kataglyphis::gltfLoadOptionsFromEnvironment().mikktspaceTangents);
#ifndef _WIN32
        // Windows' _putenv_s removes a variable set to empty; POSIX keeps it, and empty must count as unset.
        ASSERT_EQ(setenv("KATAGLYPHIS_MIKKTSPACE_TANGENTS", "", 1), 0);
        ASSERT_NE(std::getenv("KATAGLYPHIS_MIKKTSPACE_TANGENTS"), nullptr);
        EXPECT_FALSE(Kataglyphis::gltfLoadOptionsFromEnvironment().mikktspaceTangents) << "an empty value is unset";
#endif
    }
    const Kataglyphis::TestSupport::ScopedEnvVar set("KATAGLYPHIS_MIKKTSPACE_TANGENTS", "1");
    EXPECT_TRUE(Kataglyphis::gltfLoadOptionsFromEnvironment().mikktspaceTangents);

    GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(quad));
    EXPECT_EQ(loader.getVertices().size(), 4U) << "a load with the default options read the environment itself";
}
