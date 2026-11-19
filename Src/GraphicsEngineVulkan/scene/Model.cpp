module;
#include <memory>

#include "common/Utilities.hpp"
#include "vulkan_base/PhysicalDeviceChoices.hpp"
#include <cstdint>
#include <glm/ext/matrix_float4x4.hpp>
#include <optional>
#include <span>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

module kataglyphis.vulkan.model;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.obj_material;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.mesh;
import kataglyphis.vulkan.vertex;
import kataglyphis.vulkan.sampler_builder;

using namespace Kataglyphis;

Model::Model() = default;

Model::Model(const std::shared_ptr<VulkanDevice> &device) : device(device) {}

void Model::cleanUp()
{
    if (device == nullptr) return;
    for (Texture &texture : modelTextures) { texture.cleanUp(); }
    modelTextures.clear();

    for (vk::Sampler owned_sampler : ownedSamplers) { device->getLogicalDevice().destroySampler(owned_sampler); }
    ownedSamplers.clear();
    ownedSamplerKeys.clear();
    modelTextureSamplers.clear();

    for (Mesh &m : meshes) { m.cleanUp(); }
    meshes.clear();
}

void Model::add_new_mesh(const std::shared_ptr<VulkanDevice> &vulkan_device,
  vk::CommandPool command_pool,
  std::span<const Vertex> vertices,
  std::span<const unsigned int> indices,
  std::span<const unsigned int> materialIndex,
  std::span<const ObjMaterial> materials,
  bool double_sided,
  bool alpha_blended,
  unsigned int firstVertex)
{
    // One mesh per glTF primitive or OBJ group.
    meshes.emplace_back(vulkan_device, command_pool, vertices, indices, materialIndex, materials, firstVertex);
    meshes.back().setDoubleSided(double_sided);
    meshes.back().setAlphaBlended(alpha_blended);
}

void Model::set_model(glm::mat4 new_model) { this->model = new_model; }

void Model::addTexture(Texture &&newTexture, GltfSamplerDesc samplerDesc)
{
    modelTextures.emplace_back(std::move(newTexture));
    addSampler(modelTextures.back(), samplerDesc);
}

Model::~Model() { cleanUp(); }

void Model::addSampler(const Texture &newTexture, const GltfSamplerDesc &samplerDesc)
{
    const SamplerKey key{ newTexture.getMipLevel(), samplerDesc };

    if (std::optional<std::size_t> existing = findSampler(ownedSamplerKeys, key); existing.has_value()) {
        modelTextureSamplers.push_back(ownedSamplers[*existing]);
        return;
    }

    // Not for nearest filtering, whose blocky look the author chose, nor for glTF's mipmap-less min filters.
    const bool aniso = device->supportsSamplerAnisotropy() && anisotropyAllowed(samplerDesc);

    vk::SamplerCreateInfo sampler_create_info = buildSamplerCreateInfo(samplerDesc,
      static_cast<float>(key.mipLevel),
      aniso,
      resolveMaxAnisotropy(aniso, device->maxSamplerAnisotropy()),
      vk::BorderColor::eFloatOpaqueBlack);

    vk::Sampler newSampler =
      vkCheck(device->getLogicalDevice().createSampler(sampler_create_info), "Failed to create texture sampler!");

    ownedSamplerKeys.push_back(key);
    ownedSamplers.push_back(newSampler);
    modelTextureSamplers.push_back(newSampler);
}