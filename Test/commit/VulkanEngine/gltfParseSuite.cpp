// cube.glb is the Rust renderer's asset, copied in, because both renderers must load the same glTF.

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <set>
#include <string>
#include <vulkan/vulkan.hpp>

import kataglyphis.vulkan.gltf_loader;
import kataglyphis.vulkan.vertex;
import kataglyphis.vulkan.obj_material;
import kataglyphis.vulkan.scene_config;

namespace {

std::string test_gltf() { return sceneConfig::resolveModelPath("Models/GltfTest/cube.glb"); }

std::string test_textured_gltf() { return sceneConfig::resolveModelPath("Models/GltfTest/cube_textured.gltf"); }

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

TEST(GltfParseUnit, MaterialsAndPerFaceIndexAreConsistent)
{
    if (!std::filesystem::exists(test_gltf())) { GTEST_SKIP() << "test glb not present"; }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(test_gltf()));

    EXPECT_GT(loader.getMaterials().size(), 0U) << "at least the neutral fallback";
    EXPECT_EQ(loader.getMaterialIndices().size(), loader.getIndices().size() / 3U)
      << "materialIndex is one id per triangle, like the OBJ path";
    for (unsigned int id : loader.getMaterialIndices()) {
        EXPECT_LT(id, loader.getMaterials().size()) << "every face id indexes a real material";
    }
}

TEST(GltfParseUnit, AMissingFileFailsInsteadOfCrashing)
{
    Kataglyphis::GltfLoader loader;
    EXPECT_FALSE(loader.parseCpu("this/path/does/not/exist.glb"));
    EXPECT_TRUE(loader.getVertices().empty());
    EXPECT_TRUE(loader.getIndices().empty());
}

TEST(GltfParseUnit, ExtractsAnEmbeddedBaseColorTexture)
{
    // The base-colour image is a base64 data URI; the GPU upload is loadModel's job and untested here.
    if (!std::filesystem::exists(test_textured_gltf())) { GTEST_SKIP() << "textured test gltf not present"; }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(test_textured_gltf()));

    ASSERT_EQ(loader.getTextureImages().size(), 1U) << "the one material has one base-colour texture";
    const std::vector<unsigned char> &encoded = loader.getTextureImages()[0];

    // The PNG signature proves the base64 decode landed on real image data.
    ASSERT_GE(encoded.size(), 8U);
    const unsigned char png_magic[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    for (size_t i = 0; i < 8; ++i) { EXPECT_EQ(encoded[i], png_magic[i]) << "PNG signature byte " << i; }

    // Some material points at the extracted texture, and every id is in range.
    bool anyTextured = false;
    for (const ObjMaterial &material : loader.getMaterials()) {
        if (material.textureID >= 0) {
            EXPECT_LT(static_cast<size_t>(material.textureID), loader.getTextureImages().size());
            anyTextured = true;
        }
    }
    EXPECT_TRUE(anyTextured) << "a textured material must reference the extracted texture";
}

TEST(GltfParseUnit, MaterialsSharingAnImageShareOneTextureSlot)
{
    // Both materials name images[0], so the second must reuse the first's slot rather than decode a duplicate.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "materials": [
        { "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } } },
        { "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } } }
      ],
      "textures": [ { "source": 0 } ],
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
    const auto tmp = std::filesystem::temp_directory_path() / "kat_shared_image.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_EQ(loader.getTextureImages().size(), 1U) << "the shared image must be extracted only once";
    ASSERT_EQ(loader.getMaterials().size(), 3U) << "the two declared materials, plus the neutral fallback";
    EXPECT_EQ(loader.getMaterials()[0].textureID, 0);
    EXPECT_EQ(loader.getMaterials()[1].textureID, 0) << "the second material must share slot 0, not get its own";
}

TEST(GltfParseUnit, DistinctImagesStillGetDistinctSlots)
{
    // Dedup keys on the declared image, not its bytes.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "materials": [
        { "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } } },
        { "pbrMetallicRoughness": { "baseColorTexture": { "index": 1 } } }
      ],
      "textures": [ { "source": 0 }, { "source": 1 } ],
      "images": [
        { "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFklEQVR4nGPQuGPz/47Gif8MIALEAQBX2AoVR8sp2gAAAABJRU5ErkJggg==" },
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
    const auto tmp = std::filesystem::temp_directory_path() / "kat_distinct_images.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_EQ(loader.getTextureImages().size(), 2U) << "two distinct declared images must both be extracted";
    ASSERT_EQ(loader.getMaterials().size(), 3U) << "the two declared materials, plus the neutral fallback";
    EXPECT_EQ(loader.getMaterials()[0].textureID, 0);
    EXPECT_EQ(loader.getMaterials()[1].textureID, 1);
}

TEST(GltfParseUnit, EmissiveTextureGetsItsOwnTextureSlot)
{
    // Base colour and emissive name different images.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "materials": [
        {
          "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } },
          "emissiveTexture": { "index": 1 },
          "emissiveFactor": [1, 1, 1]
        }
      ],
      "textures": [ { "source": 0 }, { "source": 1 } ],
      "images": [
        { "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFklEQVR4nGPQuGPz/47Gif8MIALEAQBX2AoVR8sp2gAAAABJRU5ErkJggg==" },
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
    const auto tmp = std::filesystem::temp_directory_path() / "kat_distinct_emissive_texture.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_EQ(loader.getTextureImages().size(), 2U) << "the two distinct declared images must both be extracted";
    ASSERT_EQ(loader.getMaterials().size(), 2U) << "the one declared material, plus the neutral fallback";
    EXPECT_EQ(loader.getMaterials()[0].textureID, 0);
    EXPECT_EQ(loader.getMaterials()[0].emissiveTextureID, 1);
    EXPECT_NE(loader.getMaterials()[0].textureID, loader.getMaterials()[0].emissiveTextureID)
      << "distinct images must not collapse onto one slot";
}

TEST(GltfParseUnit, EmissiveAndBaseColourSharingOneImageShareOneSlot)
{
    // Slots are the shared descriptor budget, so a shared sRGB image takes one.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "materials": [
        {
          "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } },
          "emissiveTexture": { "index": 0 },
          "emissiveFactor": [1, 1, 1]
        }
      ],
      "textures": [ { "source": 0 } ],
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
    const auto tmp = std::filesystem::temp_directory_path() / "kat_shared_emissive_texture.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_EQ(loader.getTextureImages().size(), 1U) << "the shared image must be extracted only once";
    ASSERT_EQ(loader.getMaterials().size(), 2U) << "the one declared material, plus the neutral fallback";
    EXPECT_EQ(loader.getMaterials()[0].textureID, 0);
    EXPECT_EQ(loader.getMaterials()[0].emissiveTextureID, 0)
      << "base-colour and emissive naming the same (image, sampler) pair must share one slot";
    ASSERT_EQ(loader.getTextureSrgbFlags().size(), loader.getTextureImages().size());
    EXPECT_EQ(loader.getTextureSrgbFlags()[0], 1)
      << "base-colour and emissive are both sRGB, so sharing a slot is still correct under the colour-space key";
}

TEST(GltfParseUnit, MaterialWithoutAnEmissiveTextureKeepsTheSentinel)
{
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "materials": [
        { "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } } }
      ],
      "textures": [ { "source": 0 } ],
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
    const auto tmp = std::filesystem::temp_directory_path() / "kat_no_emissive_texture.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_EQ(loader.getMaterials().size(), 2U) << "the one declared material, plus the neutral fallback";
    EXPECT_EQ(loader.getMaterials()[0].textureID, 0);
    EXPECT_EQ(loader.getMaterials()[0].emissiveTextureID, -1)
      << "a material without an emissiveTexture must keep the -1 sentinel";
}

TEST(GltfParseUnit, NormalTextureGetsItsOwnTextureSlot)
{
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "materials": [
        {
          "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } },
          "normalTexture": { "index": 1 }
        }
      ],
      "textures": [ { "source": 0 }, { "source": 1 } ],
      "images": [
        { "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFklEQVR4nGPQuGPz/47Gif8MIALEAQBX2AoVR8sp2gAAAABJRU5ErkJggg==" },
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
    const auto tmp = std::filesystem::temp_directory_path() / "kat_distinct_normal_texture.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_EQ(loader.getTextureImages().size(), 2U) << "the two distinct declared images must both be extracted";
    ASSERT_EQ(loader.getMaterials().size(), 2U) << "the one declared material, plus the neutral fallback";
    EXPECT_EQ(loader.getMaterials()[0].textureID, 0);
    EXPECT_EQ(loader.getMaterials()[0].normalTextureID, 1);
    EXPECT_NE(loader.getMaterials()[0].textureID, loader.getMaterials()[0].normalTextureID)
      << "distinct images must not collapse onto one slot";
    ASSERT_EQ(loader.getTextureSrgbFlags().size(), loader.getTextureImages().size());
    EXPECT_EQ(loader.getTextureSrgbFlags()[loader.getMaterials()[0].textureID], 1)
      << "base-colour texel data is sRGB-encoded";
    EXPECT_EQ(loader.getTextureSrgbFlags()[loader.getMaterials()[0].normalTextureID], 0)
      << "normal-map texel data is linear tangent-space offsets, not gamma-encoded colour";
}

TEST(GltfParseUnit, NormalAndBaseColourSharingOneImageGetTwoSlotsForDifferentColourSpaces)
{
    // A slot carries one VkFormat, and base colour (sRGB) and normal (UNORM) need different ones.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "materials": [
        {
          "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } },
          "normalTexture": { "index": 0 }
        }
      ],
      "textures": [ { "source": 0 } ],
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
    const auto tmp = std::filesystem::temp_directory_path() / "kat_shared_normal_texture.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_EQ(loader.getTextureImages().size(), 2U)
      << "the shared image must still be extracted twice: one slot can only carry one image format, and "
         "base-colour (sRGB) and normal (linear) need different formats";
    ASSERT_EQ(loader.getMaterials().size(), 2U) << "the one declared material, plus the neutral fallback";
    EXPECT_EQ(loader.getMaterials()[0].textureID, 0);
    EXPECT_EQ(loader.getMaterials()[0].normalTextureID, 1);
    EXPECT_NE(loader.getMaterials()[0].textureID, loader.getMaterials()[0].normalTextureID)
      << "one (image, sampler) pair used at two colour spaces must not collapse onto one slot";
    ASSERT_EQ(loader.getTextureSrgbFlags().size(), loader.getTextureImages().size());
    EXPECT_EQ(loader.getTextureSrgbFlags()[loader.getMaterials()[0].textureID], 1);
    EXPECT_EQ(loader.getTextureSrgbFlags()[loader.getMaterials()[0].normalTextureID], 0);
}

TEST(GltfParseUnit, MaterialWithoutANormalTextureKeepsTheSentinel)
{
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "materials": [
        { "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } } }
      ],
      "textures": [ { "source": 0 } ],
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
    const auto tmp = std::filesystem::temp_directory_path() / "kat_no_normal_texture.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_EQ(loader.getMaterials().size(), 2U) << "the one declared material, plus the neutral fallback";
    EXPECT_EQ(loader.getMaterials()[0].textureID, 0);
    EXPECT_EQ(loader.getMaterials()[0].normalTextureID, -1)
      << "a material without a normalTexture must keep the -1 sentinel";
    EXPECT_FLOAT_EQ(loader.getMaterials()[0].normalScale, 1.0F)
      << "a material without a normalTexture must keep normalScale unscaled, even though cgltf leaves "
         "cgltf_texture_view::scale zero-initialized in that case";
}

TEST(GltfParseUnit, NormalTextureScaleIsCarriedIntoTheMaterial)
{
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "materials": [
        {
          "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } },
          "normalTexture": { "index": 1, "scale": 0.5 }
        }
      ],
      "textures": [ { "source": 0 }, { "source": 1 } ],
      "images": [
        { "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFklEQVR4nGPQuGPz/47Gif8MIALEAQBX2AoVR8sp2gAAAABJRU5ErkJggg==" },
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
    const auto tmp = std::filesystem::temp_directory_path() / "kat_normal_texture_scale.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_EQ(loader.getMaterials().size(), 2U) << "the one declared material, plus the neutral fallback";
    EXPECT_FLOAT_EQ(loader.getMaterials()[0].normalScale, 0.5F);
}

TEST(GltfParseUnit, MetallicRoughnessTextureGetsItsOwnLinearSlot)
{
    // Metallic-roughness must upload linear: its G/B channels are scalars, not gamma-encoded colour.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "materials": [
        {
          "pbrMetallicRoughness": {
            "baseColorTexture": { "index": 0 },
            "metallicRoughnessTexture": { "index": 1 }
          }
        }
      ],
      "textures": [ { "source": 0 }, { "source": 1 } ],
      "images": [
        { "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFklEQVR4nGPQuGPz/47Gif8MIALEAQBX2AoVR8sp2gAAAABJRU5ErkJggg==" },
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
    const auto tmp = std::filesystem::temp_directory_path() / "kat_distinct_metallic_roughness_texture.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_EQ(loader.getTextureImages().size(), 2U) << "the two distinct declared images must both be extracted";
    ASSERT_EQ(loader.getMaterials().size(), 2U) << "the one declared material, plus the neutral fallback";
    EXPECT_EQ(loader.getMaterials()[0].textureID, 0);
    EXPECT_EQ(loader.getMaterials()[0].metallicRoughnessTextureID, 1);
    EXPECT_NE(loader.getMaterials()[0].textureID, loader.getMaterials()[0].metallicRoughnessTextureID)
      << "distinct images must not collapse onto one slot";
    ASSERT_EQ(loader.getTextureSrgbFlags().size(), loader.getTextureImages().size());
    EXPECT_EQ(loader.getTextureSrgbFlags()[loader.getMaterials()[0].textureID], 1)
      << "base-colour texel data is sRGB-encoded";
    EXPECT_EQ(loader.getTextureSrgbFlags()[loader.getMaterials()[0].metallicRoughnessTextureID], 0)
      << "metallic-roughness texel data is linear roughness/metallic scalars, not gamma-encoded colour";
}

TEST(GltfParseUnit, AMaterialWithoutAMetallicRoughnessTextureKeepsTheSentinel)
{
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "materials": [
        { "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } } }
      ],
      "textures": [ { "source": 0 } ],
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
    const auto tmp = std::filesystem::temp_directory_path() / "kat_no_metallic_roughness_texture.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_EQ(loader.getMaterials().size(), 2U) << "the one declared material, plus the neutral fallback";
    EXPECT_EQ(loader.getMaterials()[0].textureID, 0);
    EXPECT_EQ(loader.getMaterials()[0].metallicRoughnessTextureID, -1)
      << "a material without a metallicRoughnessTexture must keep the -1 sentinel";
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

TEST(GltfParseUnit, OneImageWithTwoSamplersGetsTwoSlots)
{
    // The dedup key is (image, sampler), so a different sampler must not collapse onto one slot.
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

TEST(GltfParseUnit, BaseColorFactorSurvivesATexturedMaterial)
{
    // glTF base colour is factor * texture, so the factor must survive alongside the textureID.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "materials": [
        { "pbrMetallicRoughness": {
            "baseColorFactor": [0.25, 0.5, 1.0, 1.0],
            "baseColorTexture": { "index": 0 }
        } }
      ],
      "textures": [ { "source": 0 } ],
      "images": [
        { "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFklEQVR4nGPQuGPz/47Gif8MIALEAQBX2AoVR8sp2gAAAABJRU5ErkJggg==" }
      ],
      "meshes": [ { "primitives": [ {
        "attributes": { "POSITION": 0 },
        "material": 0
      } ] } ],
      "nodes": [ { "mesh": 0 } ],
      "scenes": [ { "nodes": [ 0 ] } ],
      "accessors": [ { "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [0,0,0], "max": [1,1,0], "bufferView": 0 } ],
      "bufferViews": [ { "buffer": 0, "byteLength": 36 } ],
      "buffers": [ { "byteLength": 36,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA" } ]
    })GLTF";
    const auto tmp = std::filesystem::temp_directory_path() / "kat_base_color_factor.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_EQ(loader.getMaterials().size(), 2U) << "the one declared material, plus the neutral fallback";
    const ObjMaterial &material = loader.getMaterials()[0];
    EXPECT_GE(material.textureID, 0) << "the material must still reference its base-colour texture";
    EXPECT_NEAR(material.diffuse.x, 0.25F, 1e-5F);
    EXPECT_NEAR(material.diffuse.y, 0.5F, 1e-5F);
    EXPECT_NEAR(material.diffuse.z, 1.0F, 1e-5F);
}

// The GUI model picker feeds arbitrary files to parseCpu; gltf_parsing_fuzz_test is the broad sweep.
TEST(GltfParseUnit, MalformedTextIsRejectedNotCrashed)
{
    Kataglyphis::GltfLoader loader;

    // Truncated/garbage JSON must fail parse, not walk a half-built document.
    const auto tmp = std::filesystem::temp_directory_path() / "kat_bad_gltf.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << "{ \"asset\": { \"version\": \"2.0\" ";// unterminated
    }
    EXPECT_FALSE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);
}

TEST(GltfParseUnit, ShortBase64ImageUriDoesNotUnderflow)
{
    // A URI shorter than one base64 quad underflows the decoded-length maths (b64len/4*3 - padding).
    const char *doc = R"({
      "asset": { "version": "2.0" },
      "images": [ { "uri": "data:image/png;base64,QQ" } ],
      "textures": [ { "source": 0 } ],
      "materials": [ { "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } } } ],
      "meshes": [ { "primitives": [ {
        "attributes": { "POSITION": 0 },
        "material": 0
      } ] } ],
      "nodes": [ { "mesh": 0 } ],
      "scenes": [ { "nodes": [ 0 ] } ],
      "accessors": [ { "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [0,0,0], "max": [1,1,1], "bufferView": 0 } ],
      "bufferViews": [ { "buffer": 0, "byteLength": 36 } ],
      "buffers": [ { "byteLength": 36,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA" } ]
    })";
    const auto tmp = std::filesystem::temp_directory_path() / "kat_short_b64.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }
    Kataglyphis::GltfLoader loader;
    // ASan in CI enforces no OOB; either way the malformed URI yields no texture.
    const bool parsed = loader.parseCpu(tmp.string());
    if (parsed) {
        EXPECT_TRUE(loader.getTextureImages().empty())
          << "a sub-quad base64 URI must yield no texture, not an underflowed read";
    }
    std::filesystem::remove(tmp);
}

namespace {

// extractImageBytes never decodes, so a truncated but genuine PNG prefix is enough.
const unsigned char kMinimalPngBytes[] = {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x0D,
    0x0A,
    0x1A,
    0x0A,// PNG signature
    0x00,
    0x00,
    0x00,
    0x0D,
    0x49,
    0x48,
    0x44,
    0x52// IHDR length + tag
};

// One-triangle skeleton; only the image uri varies per test.
std::string externalImageGltfDoc(const std::string &imageUri)
{
    return "{\n"
           "  \"asset\": { \"version\": \"2.0\" },\n"
           "  \"images\": [ { \"uri\": \""
      + imageUri
      + "\" } ],\n"
        "  \"textures\": [ { \"source\": 0 } ],\n"
        "  \"materials\": [ { \"pbrMetallicRoughness\": { \"baseColorTexture\": { \"index\": 0 } } } ],\n"
        "  \"meshes\": [ { \"primitives\": [ {\n"
        "    \"attributes\": { \"POSITION\": 0 },\n"
        "    \"material\": 0\n"
        "  } ] } ],\n"
        "  \"nodes\": [ { \"mesh\": 0 } ],\n"
        "  \"scenes\": [ { \"nodes\": [ 0 ] } ],\n"
        "  \"accessors\": [ { \"componentType\": 5126, \"count\": 3, \"type\": \"VEC3\",\n"
        "                   \"min\": [0,0,0], \"max\": [1,1,1], \"bufferView\": 0 } ],\n"
        "  \"bufferViews\": [ { \"buffer\": 0, \"byteLength\": 36 } ],\n"
        "  \"buffers\": [ { \"byteLength\": 36,\n"
        "    \"uri\": \"data:application/octet-stream;base64,"
        "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA\" } ]\n"
        "}\n";
}

void writeFile(const std::filesystem::path &path, const std::string &text)
{
    std::ofstream out(path, std::ios::binary);
    out << text;
}

void writePngFile(const std::filesystem::path &path)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(kMinimalPngBytes), sizeof(kMinimalPngBytes));
}

}// namespace

TEST(GltfParseUnit, ExternalImageUriIsResolvedRelativeToTheDocument)
{
    const auto dir = std::filesystem::temp_directory_path() / "kat_gltf_ext_ok";
    std::filesystem::create_directories(dir);
    writePngFile(dir / "tex.png");
    writeFile(dir / "model.gltf", externalImageGltfDoc("tex.png"));

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "model.gltf").string()));

    ASSERT_EQ(loader.getTextureImages().size(), 1U);
    EXPECT_FALSE(loader.getTextureImages()[0].empty());
    bool anyTextured = false;
    for (const ObjMaterial &material : loader.getMaterials()) {
        if (material.textureID >= 0) {
            EXPECT_EQ(material.textureID, 0);
            anyTextured = true;
        }
    }
    EXPECT_TRUE(anyTextured);

    std::filesystem::remove_all(dir);
}

TEST(GltfParseUnit, ExternalImageUriEscapingTheDocumentDirectoryIsRejected)
{
    const auto parentDir = std::filesystem::temp_directory_path() / "kat_gltf_ext_escape";
    const auto docDir = parentDir / "doc";
    std::filesystem::create_directories(docDir);
    writePngFile(parentDir / "secret.png");
    writeFile(docDir / "model.gltf", externalImageGltfDoc("../secret.png"));

    Kataglyphis::GltfLoader loader;
    const bool parsed = loader.parseCpu((docDir / "model.gltf").string());
    if (parsed) {
        EXPECT_TRUE(loader.getTextureImages().empty())
          << "a URI that escapes the document directory must yield no texture";
        for (const ObjMaterial &material : loader.getMaterials()) { EXPECT_EQ(material.textureID, -1); }
    }

    std::filesystem::remove_all(parentDir);
}

TEST(GltfParseUnit, PercentEncodedExternalUriResolves)
{
    const auto dir = std::filesystem::temp_directory_path() / "kat_gltf_ext_percent";
    std::filesystem::create_directories(dir);
    writePngFile(dir / "my tex.png");
    writeFile(dir / "model.gltf", externalImageGltfDoc("my%20tex.png"));

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu((dir / "model.gltf").string()));

    ASSERT_EQ(loader.getTextureImages().size(), 1U);
    EXPECT_FALSE(loader.getTextureImages()[0].empty());

    std::filesystem::remove_all(dir);
}

TEST(GltfParseUnit, MissingNormalsAreComputedFlatNotDefaultedUp)
{
    // glTF requires flat normals when NORMAL is absent; an XY triangle's is (0,0,+/-1), never (0,1,0).
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
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
    const auto tmp = std::filesystem::temp_directory_path() / "kat_no_normal.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_GE(loader.getVertices().size(), 3U);
    for (const Vertex &v : loader.getVertices()) {
        const glm::vec3 n = v.normal;
        EXPECT_GT(std::abs(n.z), 0.99F) << "computed flat normal should be +/-Z for an XY-plane triangle";
        EXPECT_LT(std::abs(n.y), 0.01F) << "normal defaulted to up (0,1,0) - flat computation did not run";
    }
}

TEST(GltfParseUnit, TriangleStripIsTriangulatedNotDropped)
{
    // A 4-vertex strip (mode 5) triangulates to 6 indices.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "meshes": [ { "primitives": [ {
        "attributes": { "POSITION": 0 },
        "mode": 5
      } ] } ],
      "nodes": [ { "mesh": 0 } ],
      "scenes": [ { "nodes": [ 0 ] } ],
      "accessors": [ { "componentType": 5126, "count": 4, "type": "VEC3",
                       "min": [0,0,0], "max": [1,1,0], "bufferView": 0 } ],
      "bufferViews": [ { "buffer": 0, "byteLength": 48 } ],
      "buffers": [ { "byteLength": 48,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAACAPwAAgD8AAAAA" } ]
    })GLTF";
    const auto tmp = std::filesystem::temp_directory_path() / "kat_strip.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }
    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string())) << "a triangle-strip mesh must load, not be dropped as non-triangle";
    std::filesystem::remove(tmp);

    EXPECT_EQ(loader.getVertices().size(), 4U) << "four strip vertices";
    EXPECT_EQ(loader.getIndices().size(), 6U) << "a 4-vertex strip triangulates to 2 triangles";
    EXPECT_EQ(loader.getIndices().size() % 3U, 0U);
}

TEST(GltfParseUnit, TriangleFanIsTriangulatedAroundTheHubVertex)
{
    // Pins the fan winding (0,2,3) too; the strip pattern would give (2,1,3).
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "meshes": [ { "primitives": [ {
        "attributes": { "POSITION": 0 },
        "mode": 6
      } ] } ],
      "nodes": [ { "mesh": 0 } ],
      "scenes": [ { "nodes": [ 0 ] } ],
      "accessors": [ { "componentType": 5126, "count": 4, "type": "VEC3",
                       "min": [0,0,0], "max": [1,1,0], "bufferView": 0 } ],
      "bufferViews": [ { "buffer": 0, "byteLength": 48 } ],
      "buffers": [ { "byteLength": 48,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAACAPwAAgD8AAAAA" } ]
    })GLTF";
    const auto tmp = std::filesystem::temp_directory_path() / "kat_fan.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }
    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string())) << "a triangle-fan mesh must load, not be dropped as non-triangle";
    std::filesystem::remove(tmp);

    EXPECT_EQ(loader.getVertices().size(), 4U) << "four fan vertices";
    ASSERT_EQ(loader.getIndices().size(), 6U) << "a 4-vertex fan triangulates to 2 triangles";
    const std::vector<unsigned int> expected = { 0U, 1U, 2U, 0U, 2U, 3U };
    EXPECT_EQ(loader.getIndices(), expected)
      << "a fan must triangulate around the shared hub vertex 0, not with the strip winding";
}

TEST(GltfParseUnit, OutOfRangeIndicesDropTheirTriangleNotTheMesh)
{
    // Drop whole triangles (materialIndex is per triangle); without NORMAL the flat-normal pass is ASan's oracle.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "meshes": [ { "primitives": [ {
        "attributes": { "POSITION": 0 },
        "indices": 1
      } ] } ],
      "nodes": [ { "mesh": 0 } ],
      "scenes": [ { "nodes": [ 0 ] } ],
      "accessors": [
        { "componentType": 5126, "count": 3, "type": "VEC3",
          "min": [0,0,0], "max": [1,1,0], "bufferView": 0 },
        { "componentType": 5123, "count": 6, "type": "SCALAR", "bufferView": 1 }
      ],
      "bufferViews": [
        { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
        { "buffer": 0, "byteOffset": 36, "byteLength": 12, "target": 34963 }
      ],
      "buffers": [ { "byteLength": 48,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIABwABAAIA" } ]
    })GLTF";
    const auto tmp = std::filesystem::temp_directory_path() / "kat_oob_index.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }
    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()))
      << "an out-of-range index must drop its triangle, not fail the whole mesh";
    std::filesystem::remove(tmp);

    EXPECT_EQ(loader.getVertices().size(), 3U);
    EXPECT_EQ(loader.getIndices().size(), 3U) << "only the first (valid) triangle survives";
    EXPECT_EQ(loader.getIndices().size() % 3U, 0U);
    for (unsigned int idx : loader.getIndices()) {
        EXPECT_LT(idx, loader.getVertices().size()) << "every surviving index addresses a real vertex";
    }
    EXPECT_EQ(loader.getMaterialIndices().size(), loader.getIndices().size() / 3U)
      << "material-id count must equal the surviving triangle count";
}

namespace {

std::string skin_node_gltf(bool skinned)
{
    const std::string skinBlock = skinned ? R"GLTF(, "skin": 0)GLTF" : "";
    const std::string skinsArray = skinned ? R"GLTF("skins": [ { "joints": [ 1 ] } ],)GLTF" : "";
    return std::string(R"GLTF({
      "asset": { "version": "2.0" },
      )GLTF")
           + skinsArray + R"GLTF(
      "meshes": [ { "primitives": [ {
        "attributes": { "POSITION": 0 }
      } ] } ],
      "nodes": [ { "mesh": 0, "translation": [10, 0, 0])GLTF"
           + skinBlock + R"GLTF( }, { } ],
      "scenes": [ { "nodes": [ 0, 1 ] } ],
      "accessors": [ { "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [0,0,0], "max": [1,1,0], "bufferView": 0 } ],
      "bufferViews": [ { "buffer": 0, "byteLength": 36 } ],
      "buffers": [ { "byteLength": 36,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA" } ]
    })GLTF";
}

std::string material_gltf(const std::string &alphaSnippet)
{
    return std::string(R"GLTF({
      "asset": { "version": "2.0" },
      "materials": [ { "pbrMetallicRoughness": { "baseColorFactor": [1,1,1,1] })GLTF")
           + alphaSnippet + R"GLTF( } ],
      "meshes": [ { "primitives": [ {
        "attributes": { "POSITION": 0 },
        "material": 0
      } ] } ],
      "nodes": [ { "mesh": 0 } ],
      "scenes": [ { "nodes": [ 0 ] } ],
      "accessors": [ { "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [0,0,0], "max": [1,1,0], "bufferView": 0 } ],
      "bufferViews": [ { "buffer": 0, "byteLength": 36 } ],
      "buffers": [ { "byteLength": 36,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA" } ]
    })GLTF";
}

float first_material_cutoff(const std::string &doc, const char *tmpName)
{
    const auto tmp = std::filesystem::temp_directory_path() / tmpName;
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }
    Kataglyphis::GltfLoader loader;
    const bool parsed = loader.parseCpu(tmp.string());
    std::filesystem::remove(tmp);
    EXPECT_TRUE(parsed);
    EXPECT_GT(loader.getMaterials().size(), 0U);
    return loader.getMaterials().empty() ? 0.0F : loader.getMaterials()[0].alphaCutoff;
}

}// namespace

TEST(GltfParseUnit, SkinnedNodeTransformIsIgnored)
{
    // glTF 2.0 (Skins): a skinned mesh node's transform MUST be ignored, and there is no joint animation.
    const auto tmp = std::filesystem::temp_directory_path() / "kat_skinned_translated.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << skin_node_gltf(/*skinned=*/true);
    }
    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_GE(loader.getVertices().size(), 3U);
    for (const Vertex &v : loader.getVertices()) {
        EXPECT_LT(v.position.x, 2.0F) << "a skinned node's translation must be ignored - vertex stayed in bind pose";
    }
}

TEST(GltfParseUnit, UnskinnedNodeTransformStillApplies)
{
    // Control: the skin check must not suppress every node transform.
    const auto tmp = std::filesystem::temp_directory_path() / "kat_unskinned_translated.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << skin_node_gltf(/*skinned=*/false);
    }
    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_GE(loader.getVertices().size(), 3U);
    bool anyShifted = false;
    for (const Vertex &v : loader.getVertices()) {
        if (v.position.x > 9.0F) { anyShifted = true; }
    }
    EXPECT_TRUE(anyShifted) << "an unskinned node's translation must still apply to its vertices";
}

namespace {

std::string scaled_node_gltf(const std::string &scaleJson)
{
    return std::string(R"GLTF({
      "asset": { "version": "2.0" },
      "meshes": [ { "primitives": [ {
        "attributes": { "POSITION": 0 }
      } ] } ],
      "nodes": [ { "mesh": 0, "scale": )GLTF")
           + scaleJson + R"GLTF( } ],
      "scenes": [ { "nodes": [ 0 ] } ],
      "accessors": [ { "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [0,0,0], "max": [1,1,0], "bufferView": 0 } ],
      "bufferViews": [ { "buffer": 0, "byteLength": 36 } ],
      "buffers": [ { "byteLength": 36,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA" } ]
    })GLTF";
}

std::vector<unsigned int> indices_of_scaled_node_gltf(const std::string &scaleJson, const char *tmpName)
{
    const auto tmp = std::filesystem::temp_directory_path() / tmpName;
    {
        std::ofstream out(tmp, std::ios::binary);
        out << scaled_node_gltf(scaleJson);
    }
    Kataglyphis::GltfLoader loader;
    const bool parsed = loader.parseCpu(tmp.string());
    std::filesystem::remove(tmp);
    EXPECT_TRUE(parsed);
    return loader.getIndices();
}

}// namespace

TEST(GltfParseUnit, MirroredNodeReversesTriangleWinding)
{
    // glTF 2.0 3.7.4: a mirror must reverse winding, or MeshDrawRecorder's back-face culling hides the mesh.
    const auto indices = indices_of_scaled_node_gltf("[-1, 1, 1]", "kat_mirrored.gltf");
    const std::vector<unsigned int> expected = { 0U, 2U, 1U };
    EXPECT_EQ(indices, expected) << "a mirrored (determinant < 0) node must reverse its triangle winding";
}

TEST(GltfParseUnit, UnmirroredNodeKeepsItsWinding)
{
    // Control: the determinant check must not invert every node's triangles.
    const auto indices = indices_of_scaled_node_gltf("[1, 1, 1]", "kat_unmirrored.gltf");
    const std::vector<unsigned int> expected = { 0U, 1U, 2U };
    EXPECT_EQ(indices, expected) << "an unmirrored node must keep its original triangle winding";
}

TEST(GltfParseUnit, RotatedNodeWithTwoNegativeScalesKeepsItsWinding)
{
    // Two negative factors make a rotation, not a mirror: only the determinant's sign matters.
    const auto indices = indices_of_scaled_node_gltf("[-1, -1, 1]", "kat_rotated_not_mirrored.gltf");
    const std::vector<unsigned int> expected = { 0U, 1U, 2U };
    EXPECT_EQ(indices, expected) << "two negative scale components is a rotation (det +1), winding must be unchanged";
}

TEST(GltfParseUnit, MaskAlphaModeSetsTheCutoff)
{
    // The raster shaders discard against this cutoff; losing it renders a cut-out as a solid quad.
    const float cutoff =
      first_material_cutoff(material_gltf(R"(, "alphaMode": "MASK", "alphaCutoff": 0.5)"), "kat_mask.gltf");
    EXPECT_NEAR(cutoff, 0.5F, 1e-6F) << "MASK material must carry its alphaCutoff into ObjMaterial";
}

TEST(GltfParseUnit, OpaqueMaterialHasNoCutoff)
{
    // Otherwise every opaque glTF punches holes wherever its base-colour alpha dips.
    const float cutoff = first_material_cutoff(material_gltf(""), "kat_opaque.gltf");
    EXPECT_LT(cutoff, 0.0F) << "a non-MASK material must have alphaCutoff < 0 (never discards)";
}

TEST(GltfParseUnit, BaseColourFactorAlphaReachesTheMaterial)
{
    // baseColorFactor.a is a factor of the MASK alpha, so an untextured MASK material needs it to discard.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "materials": [
        { "pbrMetallicRoughness": { "baseColorFactor": [1.0, 1.0, 1.0, 0.25] } }
      ],
      "meshes": [ { "primitives": [ {
        "attributes": { "POSITION": 0 },
        "material": 0
      } ] } ],
      "nodes": [ { "mesh": 0 } ],
      "scenes": [ { "nodes": [ 0 ] } ],
      "accessors": [ { "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [0,0,0], "max": [1,1,0], "bufferView": 0 } ],
      "bufferViews": [ { "buffer": 0, "byteLength": 36 } ],
      "buffers": [ { "byteLength": 36,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA" } ]
    })GLTF";
    const auto tmp = std::filesystem::temp_directory_path() / "kat_base_color_factor_alpha.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_GT(loader.getMaterials().size(), 0U);
    EXPECT_NEAR(loader.getMaterials()[0].dissolve, 0.25F, 1e-6F)
      << "baseColorFactor.a must reach ObjMaterial::dissolve";
}

TEST(GltfParseUnit, OpaqueMaterialWithoutFactorStillHasFullDissolve)
{
    // Anything but 1.0 would give every plain glTF a spurious discard.
    const float dissolve = [] {
        const auto tmp = std::filesystem::temp_directory_path() / "kat_no_alpha_material.gltf";
        {
            std::ofstream out(tmp, std::ios::binary);
            out << material_gltf("");
        }
        Kataglyphis::GltfLoader loader;
        const bool parsed = loader.parseCpu(tmp.string());
        std::filesystem::remove(tmp);
        EXPECT_TRUE(parsed);
        EXPECT_GT(loader.getMaterials().size(), 0U);
        return loader.getMaterials().empty() ? 0.0F : loader.getMaterials()[0].dissolve;
    }();
    EXPECT_NEAR(dissolve, 1.0F, 1e-6F) << "a material without an explicit alpha factor must be fully opaque";
}

TEST(GltfParseUnit, MetallicFactorReachesTheMaterial)
{
    // A dropped metallicFactor renders every glTF metal as a dielectric.
    const auto path = sceneConfig::resolveModelPath("Models/GltfTest/metallic_card.gltf");
    if (!std::filesystem::exists(path)) { GTEST_SKIP() << "metallic_card fixture not present"; }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(path));

    ASSERT_GT(loader.getMaterials().size(), 0U);
    EXPECT_NEAR(loader.getMaterials()[0].metallic, 0.75F, 1e-6F)
      << "the fixture's metallicFactor must reach ObjMaterial::metallic";
}

TEST(GltfParseUnit, MaterialWithoutPbrMetallicRoughnessHasZeroMetallic)
{
    // The metallic read is gated on has_pbr_metallic_roughness, like base colour and roughness.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "materials": [ {} ],
      "meshes": [ { "primitives": [ {
        "attributes": { "POSITION": 0 },
        "material": 0
      } ] } ],
      "nodes": [ { "mesh": 0 } ],
      "scenes": [ { "nodes": [ 0 ] } ],
      "accessors": [ { "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [0,0,0], "max": [1,1,0], "bufferView": 0 } ],
      "bufferViews": [ { "buffer": 0, "byteLength": 36 } ],
      "buffers": [ { "byteLength": 36,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA" } ]
    })GLTF";
    const auto tmp = std::filesystem::temp_directory_path() / "kat_no_pbr_material.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_GT(loader.getMaterials().size(), 0U);
    EXPECT_NEAR(loader.getMaterials()[0].metallic, 0.0F, 1e-6F)
      << "a material without pbrMetallicRoughness must default to metallic 0.0";
}

TEST(GltfParseUnit, RoughnessFactorReachesTheMaterialUnchanged)
{
    // Lossless, not round-tripped through the shininess approximation.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "materials": [
        { "pbrMetallicRoughness": { "baseColorFactor": [1,1,1,1], "roughnessFactor": 0.5 } }
      ],
      "meshes": [ { "primitives": [ {
        "attributes": { "POSITION": 0 },
        "material": 0
      } ] } ],
      "nodes": [ { "mesh": 0 } ],
      "scenes": [ { "nodes": [ 0 ] } ],
      "accessors": [ { "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [0,0,0], "max": [1,1,0], "bufferView": 0 } ],
      "bufferViews": [ { "buffer": 0, "byteLength": 36 } ],
      "buffers": [ { "byteLength": 36,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA" } ]
    })GLTF";
    const auto tmp = std::filesystem::temp_directory_path() / "kat_roughness_factor.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_GT(loader.getMaterials().size(), 0U);
    EXPECT_NEAR(loader.getMaterials()[0].roughness, 0.5F, 1e-5F)
      << "the material's roughnessFactor must reach ObjMaterial::roughness";
}

TEST(GltfParseUnit, MaterialWithoutPbrMetallicRoughnessHasNoAuthoredRoughness)
{
    // The negative sentinel is what makes material_roughness() fall back to shininess.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "materials": [ {} ],
      "meshes": [ { "primitives": [ {
        "attributes": { "POSITION": 0 },
        "material": 0
      } ] } ],
      "nodes": [ { "mesh": 0 } ],
      "scenes": [ { "nodes": [ 0 ] } ],
      "accessors": [ { "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [0,0,0], "max": [1,1,0], "bufferView": 0 } ],
      "bufferViews": [ { "buffer": 0, "byteLength": 36 } ],
      "buffers": [ { "byteLength": 36,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA" } ]
    })GLTF";
    const auto tmp = std::filesystem::temp_directory_path() / "kat_no_pbr_material_roughness.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_GT(loader.getMaterials().size(), 0U);
    EXPECT_LT(loader.getMaterials()[0].roughness, 0.0F)
      << "a material without pbrMetallicRoughness must have no authored roughness";
}

TEST(GltfParseUnit, GltfShininessIsThePinnedFallbackValue)
{
    // Shininess is read only when no roughness is authored, so kFallbackShininess ignores roughnessFactor.
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
    EXPECT_LT(withoutPbr.roughness, 0.0F)
      << "the material without pbrMetallicRoughness must have no authored roughness";

    EXPECT_FLOAT_EQ(withPbr.shininess, 1.0F)
      << "shininess must be the pinned fallback value regardless of roughnessFactor";
    EXPECT_FLOAT_EQ(withoutPbr.shininess, 1.0F)
      << "shininess must be the pinned fallback value regardless of roughnessFactor";
}

TEST(GltfParseUnit, EmissiveFactorReachesTheMaterial)
{
    const auto path = sceneConfig::resolveModelPath("Models/GltfTest/emissive_card.gltf");
    if (!std::filesystem::exists(path)) { GTEST_SKIP() << "emissive_card fixture not present"; }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(path));

    ASSERT_GT(loader.getMaterials().size(), 0U);
    const glm::vec3 &emission = loader.getMaterials()[0].emission;
    EXPECT_NEAR(emission.x, 1.0F, 1e-6F) << "the fixture's emissiveFactor must reach ObjMaterial::emission";
    EXPECT_NEAR(emission.y, 1.0F, 1e-6F) << "the fixture's emissiveFactor must reach ObjMaterial::emission";
    EXPECT_NEAR(emission.z, 1.0F, 1e-6F) << "the fixture's emissiveFactor must reach ObjMaterial::emission";
}

TEST(GltfParseUnit, EmissiveStrengthScalesTheEmissiveFactor)
{
    // The strength must be folded in, since emissiveFactor alone is clamped to [0,1].
    const auto path = sceneConfig::resolveModelPath("Models/GltfTest/emissive_strength_card.gltf");
    if (!std::filesystem::exists(path)) { GTEST_SKIP() << "emissive_strength_card fixture not present"; }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(path));

    ASSERT_GT(loader.getMaterials().size(), 0U);
    const glm::vec3 &emission = loader.getMaterials()[0].emission;
    EXPECT_NEAR(emission.x, 4.0F, 1e-5F) << "emissiveStrength must scale emissiveFactor into ObjMaterial::emission";
    EXPECT_NEAR(emission.y, 4.0F, 1e-5F) << "emissiveStrength must scale emissiveFactor into ObjMaterial::emission";
    EXPECT_NEAR(emission.z, 4.0F, 1e-5F) << "emissiveStrength must scale emissiveFactor into ObjMaterial::emission";
}

TEST(GltfParseUnit, MaterialWithoutEmissiveStrengthIsUnscaled)
{
    // Without has_emissive_strength, emission stays at the plain factor.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "materials": [
        { "emissiveFactor": [0.2, 0.4, 0.6] }
      ],
      "meshes": [ { "primitives": [ {
        "attributes": { "POSITION": 0 },
        "material": 0
      } ] } ],
      "nodes": [ { "mesh": 0 } ],
      "scenes": [ { "nodes": [ 0 ] } ],
      "accessors": [ { "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [0,0,0], "max": [1,1,0], "bufferView": 0 } ],
      "bufferViews": [ { "buffer": 0, "byteLength": 36 } ],
      "buffers": [ { "byteLength": 36,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA" } ]
    })GLTF";
    const auto tmp = std::filesystem::temp_directory_path() / "kat_no_emissive_strength_material.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_GT(loader.getMaterials().size(), 0U);
    const glm::vec3 &emission = loader.getMaterials()[0].emission;
    EXPECT_NEAR(emission.x, 0.2F, 1e-6F) << "a material without emissive_strength must keep the plain factor";
    EXPECT_NEAR(emission.y, 0.4F, 1e-6F) << "a material without emissive_strength must keep the plain factor";
    EXPECT_NEAR(emission.z, 0.6F, 1e-6F) << "a material without emissive_strength must keep the plain factor";
}

TEST(GltfParseUnit, KhrMaterialsUnlitReachesTheMaterial)
{
    // Both sides of the per-material flag in one parse, so mixing them up shows as equal values.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "materials": [
        { "extensions": { "KHR_materials_unlit": {} } },
        {}
      ],
      "meshes": [
        { "primitives": [ { "attributes": { "POSITION": 0 }, "material": 0 } ] },
        { "primitives": [ { "attributes": { "POSITION": 0 }, "material": 1 } ] }
      ],
      "nodes": [ { "mesh": 0 }, { "mesh": 1 } ],
      "scenes": [ { "nodes": [ 0, 1 ] } ],
      "accessors": [ { "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [0,0,0], "max": [1,1,0], "bufferView": 0 } ],
      "bufferViews": [ { "buffer": 0, "byteLength": 36 } ],
      "buffers": [ { "byteLength": 36,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA" } ]
    })GLTF";
    const auto tmp = std::filesystem::temp_directory_path() / "kat_unlit_material.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_GE(loader.getMaterials().size(), 2U);
    EXPECT_EQ(loader.getMaterials()[0].unlit, 1) << "KHR_materials_unlit must reach ObjMaterial::unlit";
    EXPECT_EQ(loader.getMaterials()[1].unlit, 0) << "a material without the extension must default to lit";
}

TEST(GltfParseUnit, MaskCardFixtureLoadsWithCutoutTextureAndCutoff)
{
    // The MASK goldens build on mask_card, so a failing golden must mean the renderer, not a broken fixture.
    const auto path = sceneConfig::resolveModelPath("Models/GltfTest/mask_card.gltf");
    if (!std::filesystem::exists(path)) { GTEST_SKIP() << "mask_card fixture not present"; }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(path));

    EXPECT_EQ(loader.getVertices().size(), 4U) << "a quad card has four corners";
    EXPECT_EQ(loader.getIndices().size(), 6U) << "two triangles";

    ASSERT_GT(loader.getMaterials().size(), 0U);
    EXPECT_NEAR(loader.getMaterials()[0].alphaCutoff, 0.5F, 1e-6F)
      << "the fixture's MASK cutoff must reach ObjMaterial";

    ASSERT_EQ(loader.getTextureImages().size(), 1U) << "the one cut-out base-colour texture";
    const std::vector<unsigned char> &png = loader.getTextureImages()[0];
    ASSERT_GE(png.size(), 8U);
    const unsigned char png_magic[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    for (size_t i = 0; i < 8; ++i) { EXPECT_EQ(png[i], png_magic[i]) << "PNG signature byte " << i; }
}

TEST(GltfParseUnit, HighContrastMaskCardExtractsItsTexture)
{
    // Separates extraction from upload: a pass here puts a missing texture on the device side.
    const auto path = sceneConfig::resolveModelPath("Models/GltfTest/mask_card_hc.gltf");
    if (!std::filesystem::exists(path)) { GTEST_SKIP() << "mask_card_hc fixture not present"; }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(path));
    ASSERT_GT(loader.getMaterials().size(), 0U);

    EXPECT_NEAR(loader.getMaterials()[0].alphaCutoff, 0.5F, 1e-6F) << "MASK cutoff must survive";
    EXPECT_GE(loader.getMaterials()[0].get_textureID(), 0)
      << "the base-colour texture must extract and set a valid textureID (else the card "
         "samples the diffuse fallback - the white-in-PT symptom)";
    EXPECT_EQ(loader.getTextureImages().size(), 1U) << "exactly one extracted base-colour image";
}

TEST(GltfParseUnit, ReadsKhrTextureTransformScale)
{
    // The fixture scales base-colour UVs by 4; identity rows would mean the texture never tiles.
    const auto path = sceneConfig::resolveModelPath("Models/GltfTest/uv_transform_card.gltf");
    if (!std::filesystem::exists(path)) { GTEST_SKIP() << "uv_transform fixture not present"; }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(path));
    ASSERT_GT(loader.getMaterials().size(), 0U);

    const ObjMaterial &material = loader.getMaterials()[0];
    EXPECT_NEAR(material.uv_transform_row0.x, 4.0F, 1e-6F) << "KHR_texture_transform scale.x must reach ObjMaterial";
    EXPECT_NEAR(material.uv_transform_row0.y, 0.0F, 1e-6F) << "no rotation -> row0.y stays 0";
    EXPECT_NEAR(material.uv_transform_row0.z, 0.0F, 1e-6F) << "absent offset.x defaults to 0";
    EXPECT_NEAR(material.uv_transform_row1.x, 0.0F, 1e-6F) << "no rotation -> row1.x stays 0";
    EXPECT_NEAR(material.uv_transform_row1.y, 4.0F, 1e-6F) << "KHR_texture_transform scale.y must reach ObjMaterial";
    EXPECT_NEAR(material.uv_transform_row1.z, 0.0F, 1e-6F) << "absent offset.y defaults to 0";
}

TEST(GltfParseUnit, KhrTextureTransformRotationReachesTheMaterial)
{
    // A flipped rotation sign gives a plausible but mirrored image, so pin it to the Rust loader's convention.
    const auto path = sceneConfig::resolveModelPath("Models/GltfTest/uv_transform_rotation_card.gltf");
    if (!std::filesystem::exists(path)) { GTEST_SKIP() << "uv_transform_rotation fixture not present"; }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(path));
    ASSERT_GT(loader.getMaterials().size(), 0U);

    const ObjMaterial &material = loader.getMaterials()[0];
    const float rotation = 1.5707963267948966F;// +pi/2, matches the fixture

    // Independent re-derivation of the spec's T*R*S, not the loader's own output.
    const float cosR = std::cos(-rotation);
    const float sinR = std::sin(-rotation);
    const glm::vec2 uv(1.0F, 0.0F);
    const glm::vec2 expected(cosR * uv.x - sinR * uv.y, sinR * uv.x + cosR * uv.y);

    const glm::vec2 actual(glm::dot(glm::vec3(uv, 1.0F), material.uv_transform_row0),
      glm::dot(glm::vec3(uv, 1.0F), material.uv_transform_row1));
    EXPECT_NEAR(actual.x, expected.x, 1e-5F) << "rotated UV.x must match the spec T*R*S formula";
    EXPECT_NEAR(actual.y, expected.y, 1e-5F) << "rotated UV.y must match the spec T*R*S formula";

    // An un-negated rotation would land on (0,1) instead.
    EXPECT_NEAR(actual.x, 0.0F, 1e-5F) << "sign pin vs. the Rust loader's rotate(-rotation) convention";
    EXPECT_NEAR(actual.y, -1.0F, 1e-5F) << "sign pin vs. the Rust loader's rotate(-rotation) convention";
}

TEST(GltfParseUnit, MaterialWithoutTextureTransformIsIdentity)
{
    const auto path = sceneConfig::resolveModelPath("Models/GltfTest/mask_card.gltf");
    if (!std::filesystem::exists(path)) { GTEST_SKIP() << "mask_card fixture not present"; }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(path));
    ASSERT_GT(loader.getMaterials().size(), 0U);

    const ObjMaterial &material = loader.getMaterials()[0];
    EXPECT_NEAR(material.uv_transform_row0.x, 1.0F, 1e-6F);
    EXPECT_NEAR(material.uv_transform_row0.y, 0.0F, 1e-6F);
    EXPECT_NEAR(material.uv_transform_row0.z, 0.0F, 1e-6F);
    EXPECT_NEAR(material.uv_transform_row1.x, 0.0F, 1e-6F);
    EXPECT_NEAR(material.uv_transform_row1.y, 1.0F, 1e-6F);
    EXPECT_NEAR(material.uv_transform_row1.z, 0.0F, 1e-6F);
}

TEST(GltfParseUnit, KhrTextureTransformIsReadPerTextureSlot)
{
    // KHR_texture_transform is per textureInfo, so one slot's transform must not leak onto the others.
    const auto path = sceneConfig::resolveModelPath("Models/GltfTest/uv_transform_slots_card.gltf");
    if (!std::filesystem::exists(path)) { GTEST_SKIP() << "uv_transform_slots fixture not present"; }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(path));
    ASSERT_GT(loader.getMaterials().size(), 0U);

    const ObjMaterial &material = loader.getMaterials()[0];
    EXPECT_NEAR(material.uv_transform_row0.x, 4.0F, 1e-6F) << "base-colour scale must reach the base-colour rows";
    EXPECT_NEAR(material.normal_uv_transform_row0.x, 2.0F, 1e-6F)
      << "normal scale must reach the normal slot's own rows, not the base-colour rows";
    EXPECT_NEAR(material.normal_uv_transform_row1.y, 2.0F, 1e-6F)
      << "normal scale must reach the normal slot's own rows, not the base-colour rows";

    EXPECT_NEAR(material.emissive_uv_transform_row0.x, 1.0F, 1e-6F) << "no transform on emissive -> identity";
    EXPECT_NEAR(material.emissive_uv_transform_row0.y, 0.0F, 1e-6F) << "no transform on emissive -> identity";
    EXPECT_NEAR(material.emissive_uv_transform_row1.x, 0.0F, 1e-6F) << "no transform on emissive -> identity";
    EXPECT_NEAR(material.emissive_uv_transform_row1.y, 1.0F, 1e-6F) << "no transform on emissive -> identity";

    EXPECT_NEAR(material.metallic_roughness_uv_transform_row0.x, 1.0F, 1e-6F)
      << "no metallic-roughness texture -> identity";
    EXPECT_NEAR(material.metallic_roughness_uv_transform_row0.y, 0.0F, 1e-6F)
      << "no metallic-roughness texture -> identity";
    EXPECT_NEAR(material.metallic_roughness_uv_transform_row1.x, 0.0F, 1e-6F)
      << "no metallic-roughness texture -> identity";
    EXPECT_NEAR(material.metallic_roughness_uv_transform_row1.y, 1.0F, 1e-6F)
      << "no metallic-roughness texture -> identity";
}

TEST(GltfParseUnit, ATransformOnANonBaseSlotAloneStillReachesTheMaterial)
{
    // No base-colour texture at all, so the normal slot's transform must be read on its own.
    const auto path = sceneConfig::resolveModelPath("Models/GltfTest/uv_transform_normal_only_card.gltf");
    if (!std::filesystem::exists(path)) { GTEST_SKIP() << "uv_transform_normal_only fixture not present"; }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(path));
    ASSERT_GT(loader.getMaterials().size(), 0U);

    const ObjMaterial &material = loader.getMaterials()[0];
    EXPECT_NEAR(material.normal_uv_transform_row0.x, 3.0F, 1e-6F)
      << "a transform declared only on the normal slot must still reach normal_uv_transform_row0";
    EXPECT_NEAR(material.normal_uv_transform_row1.y, 3.0F, 1e-6F)
      << "a transform declared only on the normal slot must still reach normal_uv_transform_row1";

    // Every other slot has no texture at all, so it must stay identity.
    EXPECT_NEAR(material.uv_transform_row0.x, 1.0F, 1e-6F) << "no base-colour texture -> identity";
    EXPECT_NEAR(material.uv_transform_row1.y, 1.0F, 1e-6F) << "no base-colour texture -> identity";
}

TEST(GltfParseUnit, AllFourTextureSlotsRoundTripTogether)
{
    // Distinct scales on all four slots catch cross-wiring that per-slot tests cannot.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "extensionsUsed": [ "KHR_texture_transform" ],
      "materials": [
        {
          "pbrMetallicRoughness": {
            "baseColorTexture": { "index": 0, "extensions": { "KHR_texture_transform": { "scale": [4, 4] } } },
            "metallicRoughnessTexture": { "index": 1, "extensions": { "KHR_texture_transform": { "scale": [3, 3] } } }
          },
          "normalTexture": { "index": 2, "extensions": { "KHR_texture_transform": { "scale": [2, 2] } } },
          "emissiveTexture": { "index": 3, "extensions": { "KHR_texture_transform": { "scale": [1.5, 1.5] } } },
          "emissiveFactor": [1, 1, 1]
        }
      ],
      "textures": [ { "source": 0 }, { "source": 1 }, { "source": 2 }, { "source": 3 } ],
      "images": [
        { "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFklEQVR4nGPQuGPz/47Gif8MIALEAQBX2AoVR8sp2gAAAABJRU5ErkJggg==" },
        { "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFklEQVR4nGPQuGPz/47Gif8MIALEAQBX2AoVR8sp2gAAAABJRU5ErkJggg==" },
        { "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFklEQVR4nGPQuGPz/47Gif8MIALEAQBX2AoVR8sp2gAAAABJRU5ErkJggg==" },
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
    const auto tmp = std::filesystem::temp_directory_path() / "kat_all_four_texture_slots.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_EQ(loader.getTextureImages().size(), 4U) << "four distinct declared images must all be extracted";
    ASSERT_EQ(loader.getMaterials().size(), 2U) << "the one declared material, plus the neutral fallback";
    const ObjMaterial &material = loader.getMaterials()[0];

    ASSERT_GE(material.textureID, 0);
    ASSERT_GE(material.metallicRoughnessTextureID, 0);
    ASSERT_GE(material.normalTextureID, 0);
    ASSERT_GE(material.emissiveTextureID, 0);
    const std::set<int> slots = {
        material.textureID, material.metallicRoughnessTextureID, material.normalTextureID, material.emissiveTextureID
    };
    EXPECT_EQ(slots.size(), 4U) << "four distinct images must land on four mutually distinct slots";

    EXPECT_NEAR(material.uv_transform_row0.x, 4.0F, 1e-6F) << "base-colour scale must reach the base-colour rows";
    EXPECT_NEAR(material.uv_transform_row1.y, 4.0F, 1e-6F) << "base-colour scale must reach the base-colour rows";
    EXPECT_NEAR(material.metallic_roughness_uv_transform_row0.x, 3.0F, 1e-6F)
      << "metallic-roughness scale must reach its own rows, not another slot's";
    EXPECT_NEAR(material.metallic_roughness_uv_transform_row1.y, 3.0F, 1e-6F)
      << "metallic-roughness scale must reach its own rows, not another slot's";
    EXPECT_NEAR(material.normal_uv_transform_row0.x, 2.0F, 1e-6F)
      << "normal scale must reach its own rows, not another slot's";
    EXPECT_NEAR(material.normal_uv_transform_row1.y, 2.0F, 1e-6F)
      << "normal scale must reach its own rows, not another slot's";
    EXPECT_NEAR(material.emissive_uv_transform_row0.x, 1.5F, 1e-6F)
      << "emissive scale must reach its own rows, not another slot's";
    EXPECT_NEAR(material.emissive_uv_transform_row1.y, 1.5F, 1e-6F)
      << "emissive scale must reach its own rows, not another slot's";

    ASSERT_EQ(loader.getTextureSrgbFlags().size(), loader.getTextureImages().size());
    EXPECT_EQ(loader.getTextureSrgbFlags()[material.textureID], 1) << "base-colour uploads sRGB";
    EXPECT_EQ(loader.getTextureSrgbFlags()[material.emissiveTextureID], 1) << "emissive uploads sRGB";
    EXPECT_EQ(loader.getTextureSrgbFlags()[material.normalTextureID], 0) << "normal uploads linear";
    EXPECT_EQ(loader.getTextureSrgbFlags()[material.metallicRoughnessTextureID], 0)
      << "metallic-roughness uploads linear";
}

TEST(GltfParseUnit, MultiPrimitiveGltfRecordsPerPrimitiveMeshRanges)
{
    // One mesh, two primitives: uploadParsed builds one Mesh per recorded MeshRange.
    const auto path = sceneConfig::resolveModelPath("Models/GltfTest/two_primitives.gltf");
    if (!std::filesystem::exists(path)) { GTEST_SKIP() << "two-primitive fixture not present"; }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(path));

    ASSERT_EQ(loader.getMeshRanges().size(), 2U) << "one MeshRange per glTF primitive";
    EXPECT_EQ(loader.getVertices().size(), 8U) << "two quads = eight vertices";
    EXPECT_EQ(loader.getIndices().size(), 12U) << "two quads = four triangles";

    const auto &r0 = loader.getMeshRanges()[0];
    const auto &r1 = loader.getMeshRanges()[1];
    EXPECT_EQ(r0.vertexBase, 0U);
    EXPECT_EQ(r0.vertexCount, 4U);
    EXPECT_EQ(r1.vertexBase, 4U) << "the second primitive's vertices follow the first";
    EXPECT_EQ(r1.vertexCount, 4U);
    // The ranges partition the flat arrays exactly.
    EXPECT_EQ(r0.indexCount + r1.indexCount, loader.getIndices().size());
    EXPECT_EQ(r0.triCount + r1.triCount, loader.getMaterialIndices().size());
    // Each primitive keeps its own material (materials 0 and 1).
    ASSERT_GT(loader.getMaterialIndices().size(), r1.triStart);
    EXPECT_EQ(loader.getMaterialIndices()[r0.triStart], 0U);
    EXPECT_EQ(loader.getMaterialIndices()[r1.triStart], 1U);
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

TEST(GltfParseUnit, ReadsColor0VertexColours)
{
    // The corners are red/green/blue/white via COLOR_0; a hardcoded white would pass none.
    const auto path = sceneConfig::resolveModelPath("Models/GltfTest/vertex_colored_quad.gltf");
    if (!std::filesystem::exists(path)) { GTEST_SKIP() << "vertex-colour fixture not present"; }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(path));
    ASSERT_EQ(loader.getVertices().size(), 4U);

    const auto has_color = [&](float r, float g, float b) {
        for (const Vertex &v : loader.getVertices()) {
            if (std::abs(v.color.x - r) < 1e-4F && std::abs(v.color.y - g) < 1e-4F && std::abs(v.color.z - b) < 1e-4F) {
                return true;
            }
        }
        return false;
    };
    EXPECT_TRUE(has_color(1.0F, 0.0F, 0.0F)) << "red corner colour missing";
    EXPECT_TRUE(has_color(0.0F, 1.0F, 0.0F)) << "green corner colour missing";
    EXPECT_TRUE(has_color(0.0F, 0.0F, 1.0F)) << "blue corner colour missing";
    EXPECT_TRUE(has_color(1.0F, 1.0F, 1.0F)) << "white corner colour missing";
}

TEST(GltfParseUnit, VertexColorAlphaIsCarriedFromColor0)
{
    // COLOR_0.a is a factor of the glTF MASK alpha product, so it must reach the vertex buffer.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "meshes": [ { "primitives": [ {
        "attributes": { "POSITION": 0, "COLOR_0": 1 },
        "mode": 5
      } ] } ],
      "nodes": [ { "mesh": 0 } ],
      "scenes": [ { "nodes": [ 0 ] } ],
      "accessors": [
        { "componentType": 5126, "count": 4, "type": "VEC3",
          "min": [0,0,0], "max": [1,1,0], "bufferView": 0 },
        { "componentType": 5126, "count": 4, "type": "VEC4", "bufferView": 1 }
      ],
      "bufferViews": [
        { "buffer": 0, "byteLength": 48 },
        { "buffer": 1, "byteLength": 64 }
      ],
      "buffers": [
        { "byteLength": 48,
          "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAACAPwAAgD8AAAAA" },
        { "byteLength": 64,
          "uri": "data:application/octet-stream;base64,AACAPwAAgD8AAIA/AACAPgAAgD8AAIA/AACAPwAAgD4AAIA/AACAPwAAgD8AAIA+AACAPwAAgD8AAIA/AACAPg==" }
      ]
    })GLTF";
    const auto tmp = std::filesystem::temp_directory_path() / "kat_color0_vec4.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_EQ(loader.getVertices().size(), 4U);
    for (const Vertex &v : loader.getVertices()) {
        EXPECT_NEAR(v.color.w, 0.25F, 1e-4F) << "VEC4 COLOR_0 alpha must reach Vertex::color's fourth component";
    }
}

TEST(GltfParseUnit, Vec3Color0DefaultsVertexAlphaToOne)
{
    // cgltf_accessor_read_float writes only 3 components here, so alpha must come from the 1.0 pre-fill.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "meshes": [ { "primitives": [ {
        "attributes": { "POSITION": 0, "COLOR_0": 1 },
        "mode": 5
      } ] } ],
      "nodes": [ { "mesh": 0 } ],
      "scenes": [ { "nodes": [ 0 ] } ],
      "accessors": [
        { "componentType": 5126, "count": 4, "type": "VEC3",
          "min": [0,0,0], "max": [1,1,0], "bufferView": 0 },
        { "componentType": 5126, "count": 4, "type": "VEC3", "bufferView": 1 }
      ],
      "bufferViews": [
        { "buffer": 0, "byteLength": 48 },
        { "buffer": 1, "byteLength": 48 }
      ],
      "buffers": [
        { "byteLength": 48,
          "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAACAPwAAgD8AAAAA" },
        { "byteLength": 48,
          "uri": "data:application/octet-stream;base64,AAAAP5qZGT8zMzM/AAAAP5qZGT8zMzM/AAAAP5qZGT8zMzM/AAAAP5qZGT8zMzM/" }
      ]
    })GLTF";
    const auto tmp = std::filesystem::temp_directory_path() / "kat_color0_vec3.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_EQ(loader.getVertices().size(), 4U);
    for (const Vertex &v : loader.getVertices()) {
        EXPECT_NEAR(v.color.w, 1.0F, 1e-6F)
          << "VEC3 COLOR_0 must default Vertex::color's alpha to 1.0, not leave it uninitialized";
        EXPECT_NEAR(v.color.x, 0.5F, 1e-4F) << "VEC3 COLOR_0's rgb must still reach Vertex::color";
    }
}

TEST(GltfParseUnit, CorruptEmbeddedImageStillAssignsDenseTextureSlots)
{
    // Image 0 fails to decode later on the device; each material must still own its dense textureID.
    const auto path = sceneConfig::resolveModelPath("Models/GltfTest/corrupt_embedded_image.gltf");
    if (!std::filesystem::exists(path)) { GTEST_SKIP() << "corrupt-embedded-image fixture not present"; }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(path));

    ASSERT_EQ(loader.getTextureImages().size(), 2U) << "both images must be extracted, corrupt or not";
    ASSERT_GE(loader.getMaterials().size(), 2U) << "the two declared materials, plus the neutral fallback";
    EXPECT_EQ(loader.getMaterials()[0].textureID, 0) << "material 0 must reference its own texture slot";
    EXPECT_EQ(loader.getMaterials()[1].textureID, 1)
      << "material 1 must reference slot 1, not be shifted down by material 0's eventual decode failure";
}

TEST(GltfParseUnit, PrimitiveWithoutMaterialRoutesToNeutralFallback)
{
    // A material-less primitive routes to a trailing neutral material so every face id stays in range.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
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
    const auto tmp = std::filesystem::temp_directory_path() / "kat_no_material.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()))
      << "a valid glTF with no material on the primitive must parse successfully";
    std::filesystem::remove(tmp);

    // The loader appends one neutral fallback material for exactly this case.
    ASSERT_EQ(loader.getMaterials().size(), 1U)
      << "no declared materials + one material-less primitive = exactly the fallback";
    ASSERT_EQ(loader.getIndices().size() % 3U, 0U) << "indices must form whole triangles";
    ASSERT_EQ(loader.getMaterialIndices().size(), loader.getIndices().size() / 3U)
      << "materialIndex is one id per triangle";

    // Every face must point at the single fallback material (index 0).
    for (size_t i = 0; i < loader.getMaterialIndices().size(); ++i) {
        const unsigned int id = loader.getMaterialIndices()[i];
        EXPECT_LT(id, loader.getMaterials().size())
          << "face material index escapes the materials array (GPU OOB read) at triangle " << i;
        EXPECT_EQ(id, 0U)
          << "a material-less primitive must use the neutral fallback (index 0), not an arbitrary material";
    }
}

TEST(GltfParseUnit, OnlyTheDefaultSceneIsLoaded)
{
    // Same rule as the Rust loader: load only the named default scene, not every node or scene 0.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "scene": 1,
      "scenes": [ { "nodes": [ 0 ] }, { "nodes": [ 1 ] } ],
      "nodes": [
        { "mesh": 0, "translation": [0, 0, 0] },
        { "mesh": 0, "translation": [100, 0, 0] }
      ],
      "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 } } ] } ],
      "accessors": [ { "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [0,0,0], "max": [1,1,0], "bufferView": 0 } ],
      "bufferViews": [ { "buffer": 0, "byteLength": 36 } ],
      "buffers": [ { "byteLength": 36,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA" } ]
    })GLTF";
    const auto tmp = std::filesystem::temp_directory_path() / "kat_default_scene.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }
    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_EQ(loader.getVertices().size(), 3U) << "only scene 1's single triangle must load";
    for (const Vertex &v : loader.getVertices()) {
        EXPECT_GT(v.position.x, 99.0F) << "the loaded triangle must carry scene 1's +100 translation";
    }
}

TEST(GltfParseUnit, NonZeroBaseColourTexCoordSetIsReported)
{
    // Vertex carries only UV set 0, so any other set must be reported unsupported.
    const Kataglyphis::TexCoordSetInfo set0 = Kataglyphis::describeTexCoordSet(0);
    EXPECT_EQ(set0.set, 0U);
    EXPECT_TRUE(set0.supported) << "TEXCOORD_0 is the one UV set Vertex carries";

    const Kataglyphis::TexCoordSetInfo set1 = Kataglyphis::describeTexCoordSet(1);
    EXPECT_EQ(set1.set, 1U);
    EXPECT_FALSE(set1.supported) << "TEXCOORD_1 has no Vertex slot to land in";
}

TEST(GltfParseUnit, ANodeNoSceneReferencesIsNotLoaded)
{
    // Node 1 is an orphan no scene references.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "scene": 0,
      "scenes": [ { "nodes": [ 0 ] } ],
      "nodes": [
        { "mesh": 0, "translation": [0, 0, 0] },
        { "mesh": 0, "translation": [100, 0, 0] }
      ],
      "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 } } ] } ],
      "accessors": [ { "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [0,0,0], "max": [1,1,0], "bufferView": 0 } ],
      "bufferViews": [ { "buffer": 0, "byteLength": 36 } ],
      "buffers": [ { "byteLength": 36,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA" } ]
    })GLTF";
    const auto tmp = std::filesystem::temp_directory_path() / "kat_orphan_node.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }
    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_EQ(loader.getVertices().size(), 3U) << "the orphan node's mesh must not load";
    for (const Vertex &v : loader.getVertices()) {
        EXPECT_LT(v.position.x, 2.0F) << "only the scene-referenced node's (untranslated) triangle must load";
    }
}

TEST(GltfParseUnit, ChildNodesOfASceneRootAreStillLoaded)
{
    // Only the meshless root is listed, so the recursion must reach its child.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "scene": 0,
      "scenes": [ { "nodes": [ 0 ] } ],
      "nodes": [
        { "children": [ 1 ] },
        { "mesh": 0, "translation": [50, 0, 0] }
      ],
      "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 } } ] } ],
      "accessors": [ { "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [0,0,0], "max": [1,1,0], "bufferView": 0 } ],
      "bufferViews": [ { "buffer": 0, "byteLength": 36 } ],
      "buffers": [ { "byteLength": 36,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA" } ]
    })GLTF";
    const auto tmp = std::filesystem::temp_directory_path() / "kat_scene_root_child.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }
    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string())) << "a scene root's mesh-bearing child must still be loaded";
    std::filesystem::remove(tmp);

    ASSERT_EQ(loader.getVertices().size(), 3U);
    for (const Vertex &v : loader.getVertices()) {
        EXPECT_GT(v.position.x, 49.0F) << "the child node's own translation must apply";
    }
}

TEST(GltfParseUnit, AuthoredTangentsArePreferredOverGeneratedOnes)
{
    // Authored tangents are kept verbatim; the untransformed node makes that exact.
    const char *doc = R"GLTF({
      "asset": { "version": "2.0" },
      "meshes": [ { "primitives": [ {
        "attributes": { "POSITION": 0, "TANGENT": 1 }
      } ] } ],
      "nodes": [ { "mesh": 0 } ],
      "scenes": [ { "nodes": [ 0 ] } ],
      "accessors": [
        { "componentType": 5126, "count": 3, "type": "VEC3",
          "min": [0,0,0], "max": [1,1,0], "bufferView": 0 },
        { "componentType": 5126, "count": 3, "type": "VEC4", "bufferView": 1 }
      ],
      "bufferViews": [
        { "buffer": 0, "byteOffset": 0, "byteLength": 36 },
        { "buffer": 1, "byteOffset": 0, "byteLength": 48 }
      ],
      "buffers": [
        { "byteLength": 36,
          "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA" },
        { "byteLength": 48,
          "uri": "data:application/octet-stream;base64,AACAPwAAAAAAAAAAAACAPwAAgD8AAAAAAAAAAAAAgD8AAIA/AAAAAAAAAAAAAIA/" }
      ]
    })GLTF";
    const auto tmp = std::filesystem::temp_directory_path() / "kat_authored_tangent.gltf";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << doc;
    }
    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(tmp.string()));
    std::filesystem::remove(tmp);

    ASSERT_EQ(loader.getVertices().size(), 3U);
    for (const Vertex &v : loader.getVertices()) {
        EXPECT_NEAR(v.tangent.x, 1.0F, 1e-5F);
        EXPECT_NEAR(v.tangent.y, 0.0F, 1e-5F);
        EXPECT_NEAR(v.tangent.z, 0.0F, 1e-5F);
        EXPECT_NEAR(v.tangent.w, 1.0F, 1e-5F) << "authored handedness must survive unmirrored/untransformed";
    }
}

TEST(GltfParseUnit, MissingTangentsAreGeneratedWithUnitLengthAndOrthogonalToTheNormal)
{
    // A real asset, not just a hand-built triangle.
    if (!std::filesystem::exists(test_gltf())) { GTEST_SKIP() << "test glb not present"; }

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(test_gltf()));

    ASSERT_GT(loader.getVertices().size(), 0U);
    for (const Vertex &v : loader.getVertices()) {
        const float tangentLen = glm::length(glm::vec3(v.tangent));
        EXPECT_NEAR(tangentLen, 1.0F, 1e-3F) << "generated tangent must be unit length";
        EXPECT_NEAR(glm::dot(glm::vec3(v.tangent), v.normal), 0.0F, 1e-3F)
          << "generated tangent must be orthogonal to the vertex normal";
        EXPECT_TRUE(v.tangent.w == 1.0F || v.tangent.w == -1.0F) << "handedness must be exactly +-1";
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
