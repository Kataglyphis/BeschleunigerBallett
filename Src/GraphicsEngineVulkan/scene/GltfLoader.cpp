module;

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan.hpp>

#include <glm/glm.hpp>

#include "common/EnvironmentVariable.hpp"
#include "spdlog/spdlog.h"

module kataglyphis.vulkan.gltf_loader;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.model;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.mesh_range;
import kataglyphis.vulkan.model_assembly;
import kataglyphis.vulkan.sampler_builder;

namespace Kataglyphis {

GltfLoader::GltfLoader(std::shared_ptr<VulkanDevice> in_device,// DEVICE_SINK_OK: moved into member
  vk::CommandPool in_command_pool)
  : device(std::move(in_device)), command_pool(in_command_pool)
{}

GltfLoader::GltfLoader(GltfLoader &&other) noexcept : device(std::move(other.device)), command_pool(other.command_pool)
{ adoptParsed(std::move(other)); }

GltfLoader &GltfLoader::operator=(GltfLoader &&other) noexcept
{
    if (this == &other) { return *this; }
    device = std::move(other.device);
    command_pool = other.command_pool;
    adoptParsed(std::move(other));
    return *this;
}

GltfLoadOptions gltfLoadOptionsFromEnvironment()
{ return GltfLoadOptions{ .mikktspaceTangents = environmentFlag("KATAGLYPHIS_MIKKTSPACE_TANGENTS") }; }

std::shared_ptr<Model> GltfLoader::loadModel(const std::string &modelFile, const GltfLoadOptions &options)
{
    if (!parseCpu(modelFile, options)) {
        spdlog::error("Failed to parse glTF: {}", modelFile);
        return nullptr;
    }
    return uploadParsed();
}

void GltfLoader::adoptParsed(GltfLoader &&other)
{
    bridge = std::move(other.bridge);
    vertices = other.vertices;
    indices = other.indices;
    materials = std::move(other.materials);
    materialIndex = other.materialIndex;
    textures = std::move(other.textures);
    textureSamplerDescs = std::move(other.textureSamplerDescs);
    textureSrgb = std::move(other.textureSrgb);
    meshRanges = std::move(other.meshRanges);
    punctualLights = std::move(other.punctualLights);
    sun = other.sun;
    metadata = std::move(other.metadata);
    // The moved-from loader reads as unparsed, not as a second view of these arrays.
    other.clearParsed();
}

std::shared_ptr<Model> GltfLoader::uploadParsed()
{
    if (!uploadPreconditionsMet(device, vertices.size(), "GltfLoader")) { return nullptr; }

    std::shared_ptr<Model> model = std::make_shared<Model>(device);

    // Upload order must match the textureIDs parseCpu assigned.
    for (std::size_t i = 0; i < textures.size(); ++i) {
        const ParsedTexture &parsed = textures[i];
        const bool srgb = textureSrgb[i] != 0;
        Texture texture;
        // levelCount 0 is a format only stb reads, its file's bytes.
        const bool created =
          parsed.levelCount > 0
            ? texture.createFromLevels(
                device, command_pool, parsed.width, parsed.height, parsed.levelCount, parsed.rgba8, srgb)
            : texture.createFromMemory(device, command_pool, parsed.encoded.data(), parsed.encoded.size(), srgb);
        addTextureOrDefault(*model, device, command_pool, created, std::move(texture), textureSamplerDescs[i]);
    }
    ensureAtLeastOneTexture(*model, device, command_pool);

    // One mesh per glTF primitive; each shares the full materials array (materialIndex keeps original indices).
    addMeshesForRangesInPlace(
      *model, device, command_pool, vertices, getIndices(), getMaterialIndices(), materials, meshRanges);
    model->setPunctualLights(punctualLights);
    model->setSun(sun);
    model->setMetadata(metadata);
    return model;
}

void GltfLoader::clearParsed()
{
    vertices = {};
    indices = {};
    materials.clear();
    materialIndex = {};
    textures.clear();
    textureSamplerDescs.clear();
    textureSrgb.clear();
    meshRanges.clear();
    punctualLights.clear();
    sun.reset();
    metadata = {};
    bridge.reset();
}

bool GltfLoader::parseCpu(const std::string &modelFile, const GltfLoadOptions &options)
{
    // A reused loader starts empty, and a failed parse leaves it empty.
    clearParsed();
    const bool parsed = parseWithRust(modelFile, options);
    if (!parsed) { clearParsed(); }
    return parsed;
}

}// namespace Kataglyphis
