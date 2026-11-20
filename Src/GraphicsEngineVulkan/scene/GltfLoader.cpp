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

#include "spdlog/spdlog.h"

module kataglyphis.vulkan.gltf_loader;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.model;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.mesh_range;
import kataglyphis.vulkan.model_assembly;
import kataglyphis.vulkan.sampler_builder;

namespace Kataglyphis {

GltfLoader::GltfLoader(std::shared_ptr<VulkanDevice> device,// DEVICE_SINK_OK: moved into member
  vk::CommandPool command_pool)
  : device(std::move(device)), command_pool(command_pool)
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
{
    // Empty counts as unset, as for the engine's other knobs and OxidANT's load_options_from_env.
    const char *value = std::getenv("KATAGLYPHIS_MIKKTSPACE_TANGENTS");
    return GltfLoadOptions{ .mikktspaceTangents = value != nullptr && *value != '\0' };
}

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
    textureImages = std::move(other.textureImages);
    textureSamplerDescs = std::move(other.textureSamplerDescs);
    textureSrgb = std::move(other.textureSrgb);
    meshRanges = std::move(other.meshRanges);
    punctualLights = std::move(other.punctualLights);
    sun = other.sun;
    // The moved-from loader reads as unparsed, not as a second view of these arrays.
    other.clearParsed();
}

std::shared_ptr<Model> GltfLoader::uploadParsed()
{
    if (!uploadPreconditionsMet(device, vertices.size(), "GltfLoader")) { return nullptr; }

    std::shared_ptr<Model> model = std::make_shared<Model>(device);

    // Upload order must match the textureIDs parseCpu assigned.
    for (std::size_t i = 0; i < textureImages.size(); ++i) {
        const std::span<const unsigned char> encoded = textureImages[i];
        Texture texture;
        const bool created =
          texture.createFromMemory(device, command_pool, encoded.data(), encoded.size(), textureSrgb[i] != 0);
        addTextureOrDefault(*model, device, command_pool, created, std::move(texture), textureSamplerDescs[i]);
    }
    ensureAtLeastOneTexture(*model, device, command_pool);

    // One mesh per glTF primitive; each shares the full materials array (materialIndex keeps original indices).
    addMeshesForRangesInPlace(
      *model, device, command_pool, vertices, getIndices(), getMaterialIndices(), materials, meshRanges);
    model->setPunctualLights(punctualLights);
    model->setSun(sun);
    return model;
}

void GltfLoader::clearParsed()
{
    vertices = {};
    indices = {};
    materials.clear();
    materialIndex = {};
    textureImages.clear();
    textureSamplerDescs.clear();
    textureSrgb.clear();
    meshRanges.clear();
    punctualLights.clear();
    sun.reset();
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
