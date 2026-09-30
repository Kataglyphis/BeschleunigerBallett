module;
#include <memory>

#include "spdlog/spdlog.h"
#include <cstdint>
#include <cstring>
#include <glm/ext/matrix_float4x4.hpp>
#include <glm/matrix.hpp>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "renderer/accelerationStructures/BlasGeometryLimits.hpp"

module kataglyphis.vulkan.mesh;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.obj_material;
import kataglyphis.vulkan.buffer_manager;
import kataglyphis.vulkan.vertex;

using namespace Kataglyphis;

Mesh::Mesh() = default;

void Mesh::cleanUp()
{
    vulkanBufferManager.cleanUp();
    vertexBuffer.cleanUp();
    indexBuffer.cleanUp();
    materialIdsBuffer.cleanUp();
    materialsBuffer.cleanUp();
}

Mesh::Mesh(const std::shared_ptr<VulkanDevice> &device,
  vk::CommandPool transfer_command_pool,
  const std::vector<Vertex> &vertices,
  const std::vector<uint32_t> &indices,
  const std::vector<unsigned int> &materialIndex,
  const std::vector<ObjMaterial> &materials)
  : vertex_count(static_cast<uint32_t>(vertices.size())), index_count(static_cast<uint32_t>(indices.size())),
    device(device)
{
    // Computed here, the last point the positions are on the CPU; an empty mesh keeps invalid bounds (visible).
    if (!vertices.empty()) {
        glm::vec3 minimum = vertices.front().get_position();
        glm::vec3 maximum = minimum;
        for (const Vertex &vertex : vertices) {
            minimum = glm::min(minimum, vertex.get_position());
            maximum = glm::max(maximum, vertex.get_position());
        }
        bounds = AABB{ minimum, maximum };
    }

    has_masked_material = Kataglyphis::blasGeometryNeedsAnyHit(materials);

    object_description = ObjectDescription{};
    createVertexBuffer(transfer_command_pool, vertices);
    createIndexBuffer(transfer_command_pool, indices);
    createMaterialIDBuffer(transfer_command_pool, materialIndex);
    createMaterialBuffer(transfer_command_pool, materials);

    // Release the shared staging buffer now, not at mesh destruction.
    vulkanBufferManager.cleanUp();

    if (device->supportsBufferDeviceAddress()) {
        vk::BufferDeviceAddressInfo vertex_info{};
        vertex_info.buffer = vertexBuffer.getBuffer();

        vk::BufferDeviceAddressInfo index_info{};
        index_info.buffer = indexBuffer.getBuffer();

        vk::BufferDeviceAddressInfo material_index_info{};
        material_index_info.buffer = materialIdsBuffer.getBuffer();

        vk::BufferDeviceAddressInfo material_info{};
        material_info.buffer = materialsBuffer.getBuffer();

        object_description.index_address = device->getBufferDeviceAddress(index_info);
        object_description.vertex_address = device->getBufferDeviceAddress(vertex_info);
        object_description.material_index_address = device->getBufferDeviceAddress(material_index_info);
        object_description.material_address = device->getBufferDeviceAddress(material_info);
    }
}

Mesh::~Mesh() = default;

void Mesh::createVertexBuffer(vk::CommandPool transfer_command_pool,
  const std::vector<Vertex> &vertices)
{
    if (!uploadDeviceLocalBuffer(transfer_command_pool,
          vertexBuffer,
          vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eStorageBuffer,
          vertices)) {
        spdlog::error("Mesh::createVertexBuffer: upload failed; vertex buffer left unwritten.");
    }
}

void Mesh::createIndexBuffer(vk::CommandPool transfer_command_pool,
  const std::vector<uint32_t> &indices)
{
    if (!uploadDeviceLocalBuffer(transfer_command_pool,
          indexBuffer,
          vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eStorageBuffer,
          indices)) {
        spdlog::error("Mesh::createIndexBuffer: upload failed; index buffer left unwritten.");
    }
}

void Mesh::createMaterialIDBuffer(vk::CommandPool transfer_command_pool,
  const std::vector<unsigned int> &materialIndex)
{
    // Read as a storage buffer, never bound as an index buffer.
    if (!uploadDeviceLocalBuffer(
          transfer_command_pool, materialIdsBuffer, vk::BufferUsageFlagBits::eStorageBuffer, materialIndex)) {
        spdlog::error("Mesh::createMaterialIDBuffer: upload failed; material-ID buffer left unwritten.");
    }
}

void Mesh::createMaterialBuffer(vk::CommandPool transfer_command_pool,
  const std::vector<ObjMaterial> &materials)
{
    // Read as a storage buffer, never bound as an index buffer.
    if (!uploadDeviceLocalBuffer(
          transfer_command_pool, materialsBuffer, vk::BufferUsageFlagBits::eStorageBuffer, materials)) {
        spdlog::error("Mesh::createMaterialBuffer: upload failed; materials buffer left unwritten.");
    }
}
