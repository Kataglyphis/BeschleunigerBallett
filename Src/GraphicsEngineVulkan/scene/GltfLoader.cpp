module;

#include <cstddef>
#include <memory>
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

std::shared_ptr<Model> GltfLoader::loadModel(const std::string &modelFile)
{
    if (!parseCpu(modelFile)) {
        spdlog::error("Failed to parse glTF: {}", modelFile);
        return nullptr;
    }
    return uploadParsed();
}

void GltfLoader::adoptParsed(GltfLoader &&other)
{
    vertices = std::move(other.vertices);
    indices = std::move(other.indices);
    materials = std::move(other.materials);
    materialIndex = std::move(other.materialIndex);
    textureImages = std::move(other.textureImages);
    textureSamplerDescs = std::move(other.textureSamplerDescs);
    textureSrgb = std::move(other.textureSrgb);
    meshRanges = std::move(other.meshRanges);
    punctualLights = std::move(other.punctualLights);
}

std::shared_ptr<Model> GltfLoader::uploadParsed()
{
    if (!uploadPreconditionsMet(device, vertices.size(), "GltfLoader")) { return nullptr; }

    std::shared_ptr<Model> model = std::make_shared<Model>(device);

    // Upload order must match the textureIDs parseCpu assigned.
    for (std::size_t i = 0; i < textureImages.size(); ++i) {
        const std::vector<unsigned char> &encoded = textureImages[i];
        Texture texture;
        const bool created =
          texture.createFromMemory(device, command_pool, encoded.data(), encoded.size(), textureSrgb[i] != 0);
        addTextureOrDefault(*model, device, command_pool, created, std::move(texture), textureSamplerDescs[i]);
    }
    ensureAtLeastOneTexture(*model, device, command_pool);

    // One mesh per glTF primitive; each shares the full materials array (materialIndex keeps original indices).
    addMeshesForRanges(*model, device, command_pool, vertices, indices, materialIndex, materials, meshRanges);
    model->setPunctualLights(punctualLights);
    return model;
}

void GltfLoader::clearParsed()
{
    vertices.clear();
    indices.clear();
    materials.clear();
    materialIndex.clear();
    textureImages.clear();
    textureSamplerDescs.clear();
    textureSrgb.clear();
    meshRanges.clear();
    punctualLights.clear();
}

bool GltfLoader::parseCpu(const std::string &modelFile)
{
    // A reused loader starts empty, and a failed parse leaves it empty.
    clearParsed();
    const bool parsed = parseWithRust(modelFile);
    if (!parsed) { clearParsed(); }
    return parsed;
}

}// namespace Kataglyphis
