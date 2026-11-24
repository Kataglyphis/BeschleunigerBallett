// The C++ half of a glTF load: the mapping into the engine's types; kataglyphis_assets' tests/flat.rs tests the parser.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "GltfVariant.hpp"
#include "RepoFiles.hpp"

import kataglyphis.shared.util.utf8_path;
import kataglyphis.vulkan.gltf_loader;
import kataglyphis.vulkan.punctual_light;
import kataglyphis.vulkan.vertex;
import kataglyphis.vulkan.obj_material;
import kataglyphis.vulkan.scene_config;

namespace {

using Kataglyphis::TestSupport::oxidantGltfFixture;

std::string test_gltf() { return oxidantGltfFixture("cube.glb"); }

std::string test_textured_gltf() { return oxidantGltfFixture("cube_textured.gltf"); }

std::string test_punctual_gltf() { return oxidantGltfFixture("point_light.gltf"); }

std::string test_directional_gltf() { return sceneConfig::resolveModelPath("Models/GltfTest/directional_light.gltf"); }

}// namespace

TEST(GltfParseUnit, ParsesACubeWithoutAVulkanDevice)
{
    if (!std::filesystem::exists(test_gltf())) { GTEST_SKIP() << "test glb not present"; }

    Kataglyphis::GltfLoader loader;// no device: this is the CPU-only path
    ASSERT_TRUE(loader.parseCpu(test_gltf()));

    EXPECT_GT(loader.getVertices().size(), 0U);
    EXPECT_GT(loader.getIndices().size(), 0U);
    EXPECT_EQ(loader.getIndices().size() % 3U, 0U) << "indices must form whole triangles";
    EXPECT_GE(loader.getVertices().size(), 8U) << "a cube has at least eight corners";
}

// The loader views the bridge's arrays instead of copying them (about 0.5 GB for the Colosseum), so copies share them.
TEST(GltfParseUnit, LoadersShareTheBridgeArraysInsteadOfCopyingThem)
{
    if (!std::filesystem::exists(test_textured_gltf())) { GTEST_SKIP() << "textured test gltf not present"; }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(test_textured_gltf()));
    ASSERT_FALSE(loader.getVertices().empty());
    ASSERT_FALSE(loader.getTextureImages().empty());

    const Kataglyphis::GltfLoader copy = loader;
    EXPECT_EQ(copy.getVertices().data(), loader.getVertices().data());
    EXPECT_EQ(copy.getIndices().data(), loader.getIndices().data());
    EXPECT_EQ(copy.getMaterialIndices().data(), loader.getMaterialIndices().data());
    EXPECT_EQ(copy.getTextureImages()[0].data(), loader.getTextureImages()[0].data());

    // The device-owning uploader takes the worker's parse the same way, and the worker reads as unparsed after.
    const Vertex *parsed_vertices = loader.getVertices().data();
    const glm::vec3 first_position = loader.getVertices()[0].position;
    {
        Kataglyphis::GltfLoader uploader;
        uploader.adoptParsed(std::move(loader));
        EXPECT_EQ(uploader.getVertices().data(), parsed_vertices);
        EXPECT_TRUE(loader.getVertices().empty());
        EXPECT_TRUE(loader.getTextureImages().empty());
    }
    // ASan reports this read if the uploader freed what the copy still views.
    EXPECT_EQ(copy.getVertices()[0].position, first_position);
}

// A moved-from loader's views would point into the parse its destination now owns, and dangle once that goes.
TEST(GltfParseUnit, AMovedFromLoaderReadsAsUnparsed)
{
    if (!std::filesystem::exists(test_textured_gltf())) { GTEST_SKIP() << "textured test gltf not present"; }

    Kataglyphis::GltfLoader source;
    ASSERT_TRUE(source.parseCpu(test_textured_gltf()));
    ASSERT_FALSE(source.getVertices().empty());
    const glm::vec3 first_position = source.getVertices()[0].position;
    const auto expect_unparsed = [](const Kataglyphis::GltfLoader &loader, const char *how) {
        EXPECT_TRUE(loader.getVertices().empty()) << how;
        EXPECT_TRUE(loader.getIndices().empty()) << how;
        EXPECT_TRUE(loader.getMaterialIndices().empty()) << how;
        EXPECT_TRUE(loader.getMaterials().empty()) << how;
        EXPECT_TRUE(loader.getTextureImages().empty()) << how;
        EXPECT_TRUE(loader.getMeshRanges().empty()) << how;
    };

    Kataglyphis::GltfLoader assigned;
    {
        Kataglyphis::GltfLoader moved = std::move(source);
        expect_unparsed(source, "after the move constructor");
        ASSERT_FALSE(moved.getVertices().empty());
        EXPECT_EQ(moved.getVertices()[0].position, first_position);
        assigned = std::move(moved);
        expect_unparsed(moved, "after the move assignment");
    }
    ASSERT_FALSE(assigned.getVertices().empty());
    EXPECT_EQ(assigned.getVertices()[0].position, first_position);
}

TEST(GltfParseUnit, AMissingFileFailsInsteadOfCrashing)
{
    Kataglyphis::GltfLoader loader;
    EXPECT_FALSE(loader.parseCpu("this/path/does/not/exist.glb"));
    EXPECT_TRUE(loader.getVertices().empty());
    EXPECT_TRUE(loader.getIndices().empty());
}

TEST(GltfParseUnit, ReadsSamplerWrapAndFilterFromTheDocument)
{
    // getTextureSamplerDescs() is index-parallel with getTextureImages().
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "materials": [
        { "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } } }
      ],
      "textures": [ { "source": 0, "sampler": 0 } ],
      "samplers": [ { "wrapS": 33071, "wrapT": 33071, "magFilter": 9728 } ],
      "images": [
        { "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFklEQVR4nGPQuGPz/47Gif8MIALEAQBX2AoVR8sp2gAAAABJRU5ErkJggg==" }
      ],
      "meshes": [ { "primitives": [ {
        "attributes": { "POSITION": 0 }
      } ] } ],
      "nodes": [ { "mesh": 0 } ],
      "scenes": [ { "nodes": [ 0 ] } ],
      "accessors": [ { "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [0,0,0], "max": [1,1,0], "bufferView": 0 } ],
      "bufferViews": [ { "buffer": 0, "byteLength": 36 } ],
      "buffers": [ { "byteLength": 36,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA" } ]
    })GLTF";
    const auto tmp = std::filesystem::temp_directory_path() / "kat_sampler_wrap_filter.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_EQ(loader.getTextureSamplerDescs().size(), 1U);
    const Kataglyphis::GltfSamplerDesc &desc = loader.getTextureSamplerDescs()[0];
    EXPECT_EQ(desc.addressModeU, vk::SamplerAddressMode::eClampToEdge);
    EXPECT_EQ(desc.addressModeV, vk::SamplerAddressMode::eClampToEdge);
    EXPECT_EQ(desc.magFilter, vk::Filter::eNearest);
    // minFilter was not set by the document - defaults survive untouched.
    EXPECT_EQ(desc.minFilter, vk::Filter::eLinear);
    EXPECT_EQ(desc.mipmapMode, vk::SamplerMipmapMode::eLinear);
}

TEST(GltfParseUnit, MipmaplessMinFiltersReadTheBaseLevelOnly)
{
    // NEAREST (9728) and LINEAR (9729) are glTF's mipmap-less min filters; 9985 (LINEAR_MIPMAP_NEAREST) and none mip.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "materials": [
        { "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } } },
        { "pbrMetallicRoughness": { "baseColorTexture": { "index": 1 } } },
        { "pbrMetallicRoughness": { "baseColorTexture": { "index": 2 } } },
        { "pbrMetallicRoughness": { "baseColorTexture": { "index": 3 } } }
      ],
      "textures": [ { "source": 0, "sampler": 0 }, { "source": 0, "sampler": 1 }, { "source": 0, "sampler": 2 },
                    { "source": 0, "sampler": 3 } ],
      "samplers": [ { "minFilter": 9729 }, { "minFilter": 9728 }, { "minFilter": 9985 }, { "magFilter": 9729 } ],
      "images": [
        { "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFklEQVR4nGPQuGPz/47Gif8MIALEAQBX2AoVR8sp2gAAAABJRU5ErkJggg==" }
      ],
      "meshes": [ { "primitives": [ {
        "attributes": { "POSITION": 0 }
      } ] } ],
      "nodes": [ { "mesh": 0 } ],
      "scenes": [ { "nodes": [ 0 ] } ],
      "accessors": [ { "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [0,0,0], "max": [1,1,0], "bufferView": 0 } ],
      "bufferViews": [ { "buffer": 0, "byteLength": 36 } ],
      "buffers": [ { "byteLength": 36,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA" } ]
    })GLTF";
    const auto tmp = std::filesystem::temp_directory_path() / "kat_mipmapless_min_filters.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    const auto &descs = loader.getTextureSamplerDescs();
    ASSERT_EQ(descs.size(), 4U) << "one slot per (image, sampler)";
    EXPECT_FALSE(descs[0].mipmaps) << "LINEAR minification sampled the mip chain";
    EXPECT_EQ(descs[0].minFilter, vk::Filter::eLinear);
    EXPECT_FALSE(descs[1].mipmaps) << "NEAREST minification sampled the mip chain";
    EXPECT_EQ(descs[1].minFilter, vk::Filter::eNearest);
    EXPECT_TRUE(descs[2].mipmaps);
    EXPECT_EQ(descs[2].mipmapMode, vk::SamplerMipmapMode::eNearest);
    EXPECT_TRUE(descs[3].mipmaps) << "an unset min filter is glTF's trilinear default";
}

TEST(GltfParseUnit, OneImageWithTwoSamplersGetsTwoSlots)
{
    // The dedup key is (image, sampler settings, srgb), so different settings must not collapse onto one slot.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "materials": [
        { "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } } },
        { "pbrMetallicRoughness": { "baseColorTexture": { "index": 1 } } }
      ],
      "textures": [ { "source": 0, "sampler": 0 }, { "source": 0, "sampler": 1 } ],
      "samplers": [
        { "wrapS": 10497, "wrapT": 10497 },
        { "wrapS": 33071, "wrapT": 33071, "magFilter": 9728 }
      ],
      "images": [
        { "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFklEQVR4nGPQuGPz/47Gif8MIALEAQBX2AoVR8sp2gAAAABJRU5ErkJggg==" }
      ],
      "meshes": [ { "primitives": [ {
        "attributes": { "POSITION": 0 }
      } ] } ],
      "nodes": [ { "mesh": 0 } ],
      "scenes": [ { "nodes": [ 0 ] } ],
      "accessors": [ { "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [0,0,0], "max": [1,1,0], "bufferView": 0 } ],
      "bufferViews": [ { "buffer": 0, "byteLength": 36 } ],
      "buffers": [ { "byteLength": 36,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA" } ]
    })GLTF";
    const auto tmp = std::filesystem::temp_directory_path() / "kat_two_samplers_one_image.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_EQ(loader.getTextureImages().size(), 2U) << "same image, different sampler must still get two slots";
    ASSERT_EQ(loader.getTextureSamplerDescs().size(), 2U);
    ASSERT_EQ(loader.getMaterials().size(), 3U) << "the two declared materials, plus the neutral fallback";
    EXPECT_EQ(loader.getMaterials()[0].textureID, 0);
    EXPECT_EQ(loader.getMaterials()[1].textureID, 1);
    EXPECT_EQ(loader.getTextureSamplerDescs()[0].addressModeU, vk::SamplerAddressMode::eRepeat);
    EXPECT_EQ(loader.getTextureSamplerDescs()[1].addressModeU, vk::SamplerAddressMode::eClampToEdge);
    EXPECT_EQ(loader.getTextureSamplerDescs()[1].magFilter, vk::Filter::eNearest);
}

TEST(GltfParseUnit, GltfShininessIsThePinnedFallbackValue)
{
    // Shininess is pinned for every glTF material, with or without a pbr block, and ignores roughnessFactor.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "materials": [
        { "pbrMetallicRoughness": { "baseColorFactor": [1,1,1,1], "roughnessFactor": 0.1 } },
        {}
      ],
      "meshes": [ { "primitives": [
        { "attributes": { "POSITION": 0 }, "material": 0 },
        { "attributes": { "POSITION": 0 }, "material": 1 }
      ] } ],
      "nodes": [ { "mesh": 0 } ],
      "scenes": [ { "nodes": [ 0 ] } ],
      "accessors": [ { "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [0,0,0], "max": [1,1,0], "bufferView": 0 } ],
      "bufferViews": [ { "buffer": 0, "byteLength": 36 } ],
      "buffers": [ { "byteLength": 36,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA" } ]
    })GLTF";
    const auto tmp = std::filesystem::temp_directory_path() / "kat_shininess_pinned.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_GE(loader.getMaterials().size(), 2U);
    const auto &withPbr = loader.getMaterials()[0];
    const auto &withoutPbr = loader.getMaterials()[1];

    EXPECT_NEAR(withPbr.roughness, 0.1F, 1e-5F)
      << "the authored roughnessFactor must still reach ObjMaterial::roughness";
    EXPECT_FLOAT_EQ(withoutPbr.roughness, 1.0F)
      << "the material without pbrMetallicRoughness takes glTF's default roughnessFactor of 1";

    EXPECT_FLOAT_EQ(withPbr.shininess, 1.0F)
      << "shininess must be the pinned fallback value regardless of roughnessFactor";
    EXPECT_FLOAT_EQ(withoutPbr.shininess, 1.0F)
      << "shininess must be the pinned fallback value regardless of roughnessFactor";
}

TEST(GltfParseUnit, MaskCardFixtureLoadsWithCutoutTextureAndCutoff)
{
    // The MASK goldens build on mask_card, so a failing golden must mean the renderer, not a broken fixture.
    const auto path = sceneConfig::resolveModelPath("Models/GltfTest/mask_card.gltf");
    if (!std::filesystem::exists(path)) { GTEST_SKIP() << "mask_card fixture not present"; }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(path));

    EXPECT_EQ(loader.getVertices().size(), 6U) << "a quad card without NORMAL has a vertex per triangle corner";
    EXPECT_EQ(loader.getIndices().size(), 6U) << "two triangles";

    ASSERT_GT(loader.getMaterials().size(), 0U);
    EXPECT_NEAR(loader.getMaterials()[0].alphaCutoff, 0.5F, 1e-6F)
      << "the fixture's MASK cutoff must reach ObjMaterial";

    ASSERT_EQ(loader.getTextureImages().size(), 1U) << "the one cut-out base-colour texture";
    const std::span<const unsigned char> png = loader.getTextureImages()[0];
    ASSERT_GE(png.size(), 8U);
    const unsigned char png_magic[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    for (size_t i = 0; i < 8; ++i) { EXPECT_EQ(png[i], png_magic[i]) << "PNG signature byte " << i; }
}

TEST(GltfParseUnit, ReparsingTheSameLoaderDoesNotAccumulateMeshRanges)
{
    // A second parse must replace meshRanges, not append to them.
    const auto path = sceneConfig::resolveModelPath("Models/GltfTest/two_primitives.gltf");
    if (!std::filesystem::exists(path)) { GTEST_SKIP() << "two-primitive fixture not present"; }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(path));
    const std::size_t firstCount = loader.getMeshRanges().size();

    ASSERT_TRUE(loader.parseCpu(path));
    EXPECT_EQ(loader.getMeshRanges().size(), firstCount) << "reparsing must not accumulate mesh ranges";

    const auto &last = loader.getMeshRanges().back();
    EXPECT_EQ(last.indexStart + last.indexCount, loader.getIndices().size())
      << "the last range must still end exactly at the flat index array's end";
}

namespace {

// Three vertices whose TEXCOORD_1 is TEXCOORD_0 turned a quarter; textures and alphaMode are the caller's.
std::string dual_uv_triangle(const std::string &material, bool with_uv1 = true)
{
    const std::string attributes =
      with_uv1 ? R"("POSITION": 0, "TEXCOORD_0": 1, "TEXCOORD_1": 2)" : R"("POSITION": 0, "TEXCOORD_0": 1)";
    return R"({
      "asset": { "version": "2.0" },
      "extensionsUsed": [ "KHR_texture_transform" ],
      "materials": [ )"
           + material + R"( ],
      "textures": [ { "source": 0 } ],
      "images": [
        { "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFklEQVR4nGPQuGPz/47Gif8MIALEAQBX2AoVR8sp2gAAAABJRU5ErkJggg==" }
      ],
      "meshes": [ { "primitives": [ { "attributes": { )"
           + attributes + R"( }, "material": 0 } ] } ],
      "nodes": [ { "mesh": 0 } ],
      "scenes": [ { "nodes": [ 0 ] } ],
      "accessors": [
        { "componentType": 5126, "count": 3, "type": "VEC3", "min": [0,0,0], "max": [1,1,0], "bufferView": 0 },
        { "componentType": 5126, "count": 3, "type": "VEC2", "bufferView": 1 },
        { "componentType": 5126, "count": 3, "type": "VEC2", "bufferView": 2 }
      ],
      "bufferViews": [
        { "buffer": 0, "byteOffset": 0, "byteLength": 36 },
        { "buffer": 0, "byteOffset": 36, "byteLength": 24 },
        { "buffer": 0, "byteOffset": 60, "byteLength": 24 }
      ],
      "buffers": [ { "byteLength": 84,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAgD8AAAAAAAAAAAAAgD8AAIA/" } ]
    })";
}

std::optional<Kataglyphis::GltfLoader> parse_document(const std::string &doc, const char *file_name)
{
    const auto tmp = std::filesystem::temp_directory_path() / file_name;
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }
    Kataglyphis::GltfLoader loader;
    const bool parsed = loader.parseCpu(tmp.string());
    std::filesystem::remove(tmp);
    if (!parsed) { return std::nullopt; }
    return loader;
}

}// namespace

TEST(GltfParseUnit, GeneratedTangentsFollowTheNormalTexturesUvSet)
{
    // TEXCOORD_1 is TEXCOORD_0 turned a quarter, so a frame built from the wrong set points 90 degrees off.
    const auto on_uv0 =
      parse_document(dual_uv_triangle(R"({ "normalTexture": { "index": 0, "texCoord": 0 } })"), "kat_tangent_uv0.gltf");
    const auto on_uv1 =
      parse_document(dual_uv_triangle(R"({ "normalTexture": { "index": 0, "texCoord": 1 } })"), "kat_tangent_uv1.gltf");
    ASSERT_TRUE(on_uv0.has_value() && on_uv1.has_value());

    // One triangle: its frame is dP/du of the chosen set, orthogonal to the normal, with the sign of dP/dv.
    const auto frame = [](const Kataglyphis::GltfLoader &loader, bool uv1) {
        const auto vertices = loader.getVertices();
        const auto uv = [&](std::size_t i) { return uv1 ? vertices[i].texture_coords1 : vertices[i].texture_coords; };
        const glm::vec3 e1 = vertices[1].position - vertices[0].position;
        const glm::vec3 e2 = vertices[2].position - vertices[0].position;
        const glm::vec2 d1 = uv(1) - uv(0);
        const glm::vec2 d2 = uv(2) - uv(0);
        const float r = 1.0F / ((d1.x * d2.y) - (d2.x * d1.y));
        const glm::vec3 t = ((e1 * d2.y) - (e2 * d1.y)) * r;
        const glm::vec3 b = ((e2 * d1.x) - (e1 * d2.x)) * r;
        const glm::vec3 n = vertices[0].normal;
        const glm::vec3 tangent = glm::normalize(t - (n * glm::dot(n, t)));
        return glm::vec4(tangent, glm::dot(glm::cross(n, tangent), b) < 0.0F ? -1.0F : 1.0F);
    };
    ASSERT_EQ(on_uv0->getVertices().size(), 3U);
    ASSERT_EQ(on_uv1->getVertices().size(), 3U);
    const glm::vec4 expected_uv0 = frame(*on_uv0, false);
    const glm::vec4 expected_uv1 = frame(*on_uv1, true);
    EXPECT_GT(glm::length(glm::vec3(expected_uv0) - glm::vec3(expected_uv1)), 0.1F)
      << "the fixture's two UV sets must give different frames, or this test proves nothing";
    for (size_t i = 0; i < 3U; ++i) {
        EXPECT_LT(glm::length(on_uv0->getVertices()[i].tangent - expected_uv0), 1e-5F) << "vertex " << i << ", UV0";
        EXPECT_LT(glm::length(on_uv1->getVertices()[i].tangent - expected_uv1), 1e-5F) << "vertex " << i << ", UV1";
    }
}

TEST(GltfParseUnit, UploadParsedOnADeviceFreeLoaderReturnsNull)
{
    // The device check comes first, so a device-free loader that never parsed reports the device.
    if (!std::filesystem::exists(test_gltf())) { GTEST_SKIP() << "test glb not present"; }

    Kataglyphis::GltfLoader loader;// device-free constructor
    ASSERT_TRUE(loader.parseCpu(test_gltf()));

    EXPECT_EQ(loader.uploadParsed(), nullptr) << "uploadParsed must refuse to run without a device";
}

TEST(GltfParseUnit, UploadParsedBeforeAnyParseReturnsNull)
{
    Kataglyphis::GltfLoader loader;// device-free, and parseCpu never called

    EXPECT_EQ(loader.uploadParsed(), nullptr) << "uploadParsed must refuse to run before a successful parseCpu";
}

// Both lights sit on mesh-less nodes, which the mesh walk alone would skip.
TEST(GltfParseUnit, ReadsPunctualLightsPosedByTheirNodes)
{
    if (!std::filesystem::exists(test_punctual_gltf())) { GTEST_SKIP() << "test gltf not present"; }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(test_punctual_gltf()));
    const auto &lights = loader.getPunctualLights();
    ASSERT_EQ(lights.size(), 2U);

    const Kataglyphis::PunctualLight &point = lights[0];
    EXPECT_EQ(point.kind, Kataglyphis::PunctualLightKind::Point);
    EXPECT_NEAR(point.position.x, -1.2F, 1e-5F);
    EXPECT_NEAR(point.position.y, 1.0F, 1e-5F);
    EXPECT_NEAR(point.intensity, 15.0F, 1e-5F);
    EXPECT_NEAR(point.range, 6.0F, 1e-5F);
    EXPECT_NEAR(point.color.g, 0.05F, 1e-5F);

    const Kataglyphis::PunctualLight &spot = lights[1];
    EXPECT_EQ(spot.kind, Kataglyphis::PunctualLightKind::Spot);
    // Rotated -90 degrees about X, so the node's -Z points straight down.
    EXPECT_NEAR(spot.direction.y, -1.0F, 1e-4F);
    EXPECT_NEAR(spot.cosInner, std::cos(0.35F), 1e-5F);
    EXPECT_NEAR(spot.cosOuter, std::cos(0.6F), 1e-5F);
}

TEST(GltfParseUnit, PackedLightsFollowTheModelMatrixAndCarryTheirCount)
{
    Kataglyphis::PunctualLight spot;
    spot.kind = Kataglyphis::PunctualLightKind::Spot;
    spot.color = glm::vec3(1.0F, 0.5F, 0.25F);
    spot.intensity = 2.0F;
    spot.range = 10.0F;
    spot.position = glm::vec3(1.0F, 2.0F, 3.0F);
    spot.direction = glm::vec3(0.0F, 0.0F, -1.0F);
    spot.cosInner = 0.9F;
    spot.cosOuter = 0.8F;
    const std::vector<Kataglyphis::PunctualLight> lights{ spot };
    // A quarter turn about Y and a shift: the light moves and turns with the model's meshes.
    const glm::mat4 model = glm::translate(glm::mat4(1.0F), glm::vec3(10.0F, 0.0F, 0.0F))
                            * glm::rotate(glm::mat4(1.0F), glm::radians(90.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    const std::vector<Kataglyphis::PlacedPunctualLights> placed{ { lights, model } };

    const std::vector<glm::vec4> rows = Kataglyphis::packPunctualLights(placed);
    ASSERT_EQ(rows.size(), 1U + Kataglyphis::PUNCTUAL_LIGHT_ROWS);
    EXPECT_EQ(rows[0].x, 1.0F);
    EXPECT_NEAR(rows[1].x, 13.0F, 1e-4F);
    EXPECT_NEAR(rows[1].z, -1.0F, 1e-4F);
    EXPECT_EQ(rows[1].w, 2.0F) << "kind code 2 is a spot light";
    EXPECT_NEAR(rows[2].r, 2.0F, 1e-5F);
    EXPECT_NEAR(rows[2].g, 1.0F, 1e-5F);
    EXPECT_EQ(rows[2].w, 10.0F);
    EXPECT_NEAR(rows[3].x, -1.0F, 1e-4F);
    EXPECT_NEAR(rows[3].z, 0.0F, 1e-4F);
    EXPECT_EQ(rows[3].w, 0.9F);
    EXPECT_EQ(rows[4].x, 0.8F);

    EXPECT_EQ(Kataglyphis::packPunctualLights({}).size(), 1U) << "an empty scene keeps its count row";
}

// The Colosseum's 4098 lx sun was shaded as a second, unshadowed punctual light on top of the editor's own.
TEST(GltfParseUnit, TheFirstDirectionalLightIsTheSunNotAPunctualLight)
{
    if (!std::filesystem::exists(test_directional_gltf())) { GTEST_SKIP() << "test gltf not present"; }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(test_directional_gltf()));
    ASSERT_TRUE(loader.getSun().has_value()) << "the directional light did not become the Sun";
    const Kataglyphis::PunctualLight &sun = *loader.getSun();
    EXPECT_EQ(sun.kind, Kataglyphis::PunctualLightKind::Directional);
    // The node turns -Z onto (0.6, -0.48, 0.64) and scales it by 3; the Sun keeps the unit direction.
    EXPECT_NEAR(sun.direction.x, 0.6F, 1e-4F);
    EXPECT_NEAR(sun.direction.y, -0.48F, 1e-4F);
    EXPECT_NEAR(sun.direction.z, 0.64F, 1e-4F);
    EXPECT_NEAR(sun.color.r, 1.0F, 1e-5F);
    EXPECT_NEAR(sun.color.g, 0.7F, 1e-5F);
    EXPECT_NEAR(sun.color.b, 0.4F, 1e-5F);

    ASSERT_EQ(loader.getPunctualLights().size(), 1U) << "the Sun must leave the punctual lights";
    EXPECT_EQ(loader.getPunctualLights()[0].kind, Kataglyphis::PunctualLightKind::Point);
    const std::vector<Kataglyphis::PlacedPunctualLights> placed{ { loader.getPunctualLights(), glm::mat4(1.0F) } };
    const std::vector<glm::vec4> rows = Kataglyphis::packPunctualLights(placed);
    ASSERT_EQ(rows[0].x, 1.0F);
    for (std::size_t row = 1; row < rows.size(); row += Kataglyphis::PUNCTUAL_LIGHT_ROWS) {
        EXPECT_NE(rows[row].w, 3.0F) << "a directional light reached the packed punctual-light rows";
    }

    // The worker's parse reaches the uploading loader, and a reused loader forgets the last file's Sun.
    Kataglyphis::GltfLoader uploader;
    uploader.adoptParsed(std::move(loader));
    ASSERT_TRUE(uploader.getSun().has_value()) << "adoptParsed dropped the Sun";
    EXPECT_NEAR(uploader.getSun()->direction.y, -0.48F, 1e-4F);
    ASSERT_TRUE(uploader.parseCpu(test_punctual_gltf()));
    EXPECT_FALSE(uploader.getSun().has_value()) << "a file without a directional light has no Sun";
}

// The editor adopts a file's Sun, so its colour takes the editor's range at load: OxidANT's sanitize (owner 2026-11-20).
TEST(GltfParseUnit, AFilesSunArrivesInTheEditorsRanges)
{
    if (!std::filesystem::exists(test_directional_gltf())) { GTEST_SKIP() << "test gltf not present"; }
    // 1e39 overflows to infinity as an f32, the one non-finite value JSON can carry.
    const auto wild = Kataglyphis::TestSupport::GltfVariant::create(
      test_directional_gltf(), "kataglyphis_wild_sun.gltf", [](nlohmann::json &gltf) {
          gltf["extensions"]["KHR_lights_punctual"]["lights"][0]["color"] = { 3.0, -0.5, 1e39 };
      });
    ASSERT_TRUE(wild.has_value()) << "could not write the wild Sun's variant";

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(wild->string()));
    ASSERT_TRUE(loader.getSun().has_value()) << "the directional light did not become the Sun";
    const Kataglyphis::PunctualLight &sun = *loader.getSun();
    EXPECT_EQ(sun.color.r, 1.0F) << "a channel over 1 clamps to the picker's top";
    EXPECT_EQ(sun.color.g, 0.0F) << "a negative channel clamps to 0";
    EXPECT_EQ(sun.color.b, 0.92F) << "an infinite channel takes the default Sun's";
    EXPECT_NEAR(sun.direction.x, 0.6F, 1e-4F) << "a unit direction is in range already";
    EXPECT_NEAR(sun.direction.y, -0.48F, 1e-4F);
    EXPECT_NEAR(sun.direction.z, 0.64F, 1e-4F);
}

TEST(GltfParseUnit, Utf8PathBytesAreTheUtf8OfThePath)
{
    const std::vector<std::uint8_t> bytes =
      Kataglyphis::Shared::utf8PathBytes(std::filesystem::path(u8"m/\u00fcber.glb"));
    const std::vector<std::uint8_t> expected{ 'm', '/', 0xC3, 0xBC, 'b', 'e', 'r', '.', 'g', 'l', 'b' };
    EXPECT_EQ(bytes, expected);
}

// A Windows std::string path is ANSI, so an umlaut anywhere in the checkout failed every glTF load in the bridge.
TEST(GltfParseUnit, ParsesAModelUnderADirectoryWithAnUmlaut)
{
    if (!std::filesystem::exists(test_gltf())) { GTEST_SKIP() << "test glb not present"; }

    std::error_code error;
    const std::filesystem::path dir =
      std::filesystem::temp_directory_path(error) / std::filesystem::path(u8"kat_gltf_\u00fcber");
    ASSERT_FALSE(error) << error.message();
    std::filesystem::create_directories(dir, error);
    ASSERT_FALSE(error) << error.message();
    const std::filesystem::path copy = dir / "cube.glb";
    std::filesystem::copy_file(test_gltf(), copy, std::filesystem::copy_options::overwrite_existing, error);
    ASSERT_FALSE(error) << error.message();

    Kataglyphis::GltfLoader loader;
    // The engine's callers hand parseCpu a path's string(), the ANSI code page on Windows.
    const bool parsed = loader.parseCpu(copy.string());
    std::filesystem::remove_all(dir, error);
    ASSERT_TRUE(parsed) << "the loader could not open a model under a directory named with an umlaut";
    EXPECT_GE(loader.getVertices().size(), 8U);
}
