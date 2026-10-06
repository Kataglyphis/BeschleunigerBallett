module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <glm/ext/matrix_float4x4.hpp>
#include <glm/matrix.hpp>
#include <memory>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>
#include <spdlog/spdlog.h>
#include "common/Utilities.hpp"
#include "renderer/accelerationStructures/BottomLevelAccelerationStructure.hpp"
#include "renderer/accelerationStructures/BlasGeometryLimits.hpp"
#include "renderer/accelerationStructures/BlasCompaction.hpp"

module kataglyphis.vulkan.as_manager;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.vertex;
import kataglyphis.vulkan.model;
import kataglyphis.vulkan.mesh;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.command_buffer_manager;
import kataglyphis.vulkan.buffer_manager;
import kataglyphis.vulkan.object_description;

Kataglyphis::VulkanRendererInternals::ASManager::ASManager() = default;

void Kataglyphis::VulkanRendererInternals::ASManager::createASForScene(const std::shared_ptr<VulkanDevice> &device,
  vk::CommandPool commandPool,
  Kataglyphis::Scene *scene)
{
    this->vulkanDevice = device;
    if (scene == nullptr || scene->getModelCount() == 0) {
        return;
    }
    if (!createBLAS(device, commandPool, scene)) {
        spdlog::error(
          "ASManager::createASForScene: BLAS build failed; no acceleration structure this scene change; ray "
          "tracing and path tracing will keep the previous TLAS.");
        return;
    }
    createTLAS(device, commandPool, scene);
}

bool Kataglyphis::VulkanRendererInternals::ASManager::createBLAS(const std::shared_ptr<VulkanDevice> &device,
  vk::CommandPool commandPool,
  Kataglyphis::Scene *scene)
{
    // Clear old BLAS if any
    for (auto &bla : blas) {
        if (bla.vulkanAS) {
            vulkanDevice->getLogicalDevice().destroyAccelerationStructureKHR(bla.vulkanAS);
        }
        bla.vulkanBuffer.cleanUp();
    }
    blas.clear();

    std::vector<BlasInput> blas_input(scene->getModelCount());

    for (uint32_t model_index = 0; model_index < scene->getModelCount(); model_index++) {
        std::shared_ptr<Model> const mesh_model = scene->get_model_list()[model_index];
        blas_input[model_index].as_geometry.reserve(mesh_model->getMeshCount());
        blas_input[model_index].as_build_offset_info.reserve(mesh_model->getMeshCount());

        for (size_t mesh_index = 0; mesh_index < mesh_model->getMeshCount(); mesh_index++) {
            vk::AccelerationStructureGeometryKHR acceleration_structure_geometry{};
            vk::AccelerationStructureBuildRangeInfoKHR acceleration_structure_build_range_info{};

            objectToVkGeometryKHR(device,
              mesh_model->getMesh(mesh_index),
              acceleration_structure_geometry,
              acceleration_structure_build_range_info);

            blas_input[model_index].as_geometry.push_back(acceleration_structure_geometry);
            blas_input[model_index].as_build_offset_info.push_back(acceleration_structure_build_range_info);
        }
    }

    std::vector<BuildAccelerationStructure> build_as_structures;
    build_as_structures.resize(scene->getModelCount());

    vk::DeviceSize max_scratch_size = 0;

    for (unsigned int i = 0; i < scene->getModelCount(); i++) {
        vk::DeviceSize current_scratch_size = 0;
        vk::DeviceSize current_size = 0;

        createAccelerationStructureInfosBLAS(
          device, build_as_structures[i], blas_input[i], current_scratch_size, current_size);

        max_scratch_size = std::max(max_scratch_size, current_scratch_size);
    }

    VulkanBuffer scratchBuffer;

    scratchBuffer.create(device,
      max_scratch_size,
      vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress
        | vk::BufferUsageFlagBits::eTransferDst,
      vk::MemoryPropertyFlagBits::eDeviceLocal,
      vk::MemoryAllocateFlagBits::eDeviceAddress);

    vk::BufferDeviceAddressInfo scratch_buffer_device_address_info{};
    scratch_buffer_device_address_info.buffer = scratchBuffer.getBuffer();

    vk::DeviceAddress const scratch_buffer_address =
      VULKAN_HPP_DEFAULT_DISPATCHER.vkGetBufferDeviceAddress(static_cast<VkDevice>(device->getLogicalDevice()),
        reinterpret_cast<VkBufferDeviceAddressInfo *>(&scratch_buffer_device_address_info));

    vk::CommandBuffer command_buffer = Kataglyphis::VulkanRendererInternals::CommandBufferManager::beginCommandBuffer(
      device->getLogicalDevice(), commandPool);
    if (!command_buffer) {
        spdlog::error("ASManager::createBLAS: failed to begin command buffer, skipping BLAS build.");
        scratchBuffer.cleanUp();
        return false;
    }

    for (size_t i = 0; i < scene->getModelCount(); i++) {
        createSingleBlas(device, command_buffer, build_as_structures[i], scratch_buffer_address);

        // The next model's build writes the same scratch buffer, so a read-only dst scope left a WAW (sync validation).
        vk::MemoryBarrier barrier;
        barrier.srcAccessMask = vk::AccessFlagBits::eAccelerationStructureWriteKHR;
        barrier.dstAccessMask =
          vk::AccessFlagBits::eAccelerationStructureReadKHR | vk::AccessFlagBits::eAccelerationStructureWriteKHR;

        command_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eAccelerationStructureBuildKHR,
          vk::PipelineStageFlagBits::eAccelerationStructureBuildKHR,
          {},
          barrier,
          {},
          {});
    }

    bool const build_submitted = Kataglyphis::VulkanRendererInternals::CommandBufferManager::endAndSubmitCommandBuffer(
      device->getLogicalDevice(), commandPool, device->getGraphicsQueue(), command_buffer);
    if (!build_submitted) {
        spdlog::error("ASManager::createBLAS: failed to submit BLAS build commands for {} model(s); no "
                      "acceleration structures were built.",
          scene->getModelCount());
        scratchBuffer.cleanUp();
        return false;
    }

    for (auto &b : build_as_structures) { blas.emplace_back(std::move(b.single_blas)); }

    scratchBuffer.cleanUp();

    compactBLAS(device, commandPool);
    return true;
}

void Kataglyphis::VulkanRendererInternals::ASManager::compactBLAS(const std::shared_ptr<VulkanDevice> &device,
  vk::CommandPool commandPool)
{
    if (blas.empty()) { return; }
    vk::Device const logical = device->getLogicalDevice();
    auto const count = static_cast<uint32_t>(blas.size());

    // 1. Query compacted sizes; legal only after the (synchronous) build, for eAllowCompaction structures.
    vk::QueryPoolCreateInfo query_pool_create_info{};
    query_pool_create_info.queryType = vk::QueryType::eAccelerationStructureCompactedSizeKHR;
    query_pool_create_info.queryCount = count;
    vk::ResultValue<vk::QueryPool> query_pool_result = logical.createQueryPool(query_pool_create_info);
    ASSERT_VULKAN(query_pool_result.result, "Failed to create query pool for BLAS compaction!");
    vk::QueryPool const query_pool = query_pool_result.value;

    std::vector<vk::AccelerationStructureKHR> handles;
    handles.reserve(count);
    for (auto const &entry : blas) { handles.push_back(entry.vulkanAS); }

    vk::CommandBuffer query_cmd =
      Kataglyphis::VulkanRendererInternals::CommandBufferManager::beginCommandBuffer(logical, commandPool);
    if (!query_cmd) {
        spdlog::error("ASManager::compactBLAS: failed to begin query command buffer, keeping uncompacted BLAS.");
        logical.destroyQueryPool(query_pool);
        return;
    }
    query_cmd.resetQueryPool(query_pool, 0, count);
    query_cmd.writeAccelerationStructuresPropertiesKHR(
      count, handles.data(), vk::QueryType::eAccelerationStructureCompactedSizeKHR, query_pool, 0);
    bool const query_submitted = Kataglyphis::VulkanRendererInternals::CommandBufferManager::endAndSubmitCommandBuffer(
      logical, commandPool, device->getGraphicsQueue(), query_cmd);
    if (!query_submitted) {
        spdlog::error(
          "ASManager::compactBLAS: failed to submit compacted-size query commands; keeping uncompacted BLAS.");
        logical.destroyQueryPool(query_pool);
        return;
    }

    std::vector<vk::DeviceSize> compacted_sizes(count);
    vk::Result const query_result = logical.getQueryPoolResults(query_pool,
      0,
      count,
      count * sizeof(vk::DeviceSize),
      compacted_sizes.data(),
      sizeof(vk::DeviceSize),
      vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
    if (query_result != vk::Result::eSuccess) {
        spdlog::warn("ASManager: compacted-size query failed ({}); keeping uncompacted BLAS.",
          static_cast<int>(query_result));
        logical.destroyQueryPool(query_pool);
        return;
    }

    if (!Kataglyphis::compactedSizesAreUsable(compacted_sizes)) {
        spdlog::warn("ASManager: driver reported an empty or 0-byte compacted BLAS size; keeping uncompacted BLAS.");
        logical.destroyQueryPool(query_pool);
        return;
    }

    // 2. Copy each BLAS into a right-sized replacement.
    std::vector<BottomLevelAccelerationStructure> compacted(count);
    vk::CommandBuffer copy_cmd =
      Kataglyphis::VulkanRendererInternals::CommandBufferManager::beginCommandBuffer(logical, commandPool);
    if (!copy_cmd) {
        spdlog::error("ASManager::compactBLAS: failed to begin copy command buffer, keeping uncompacted BLAS.");
        logical.destroyQueryPool(query_pool);
        return;
    }
    vk::DeviceSize total_compacted = 0;
    for (uint32_t i = 0; i < count; ++i) {
        total_compacted += compacted_sizes[i];

        compacted[i].vulkanBuffer.create(device,
          compacted_sizes[i],
          vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress
            | vk::BufferUsageFlagBits::eTransferDst,
          vk::MemoryPropertyFlagBits::eDeviceLocal,
          vk::MemoryAllocateFlagBits::eDeviceAddress);

        vk::AccelerationStructureCreateInfoKHR create_info{};
        create_info.type = vk::AccelerationStructureTypeKHR::eBottomLevel;
        create_info.size = compacted_sizes[i];
        create_info.buffer = compacted[i].vulkanBuffer.getBuffer();
        vk::ResultValue<vk::AccelerationStructureKHR> compacted_as_result =
          logical.createAccelerationStructureKHR(create_info);
        ASSERT_VULKAN(compacted_as_result.result, "Failed to create compacted acceleration structure!");
        compacted[i].vulkanAS = compacted_as_result.value;

        vk::CopyAccelerationStructureInfoKHR copy_info{};
        copy_info.src = blas[i].vulkanAS;
        copy_info.dst = compacted[i].vulkanAS;
        copy_info.mode = vk::CopyAccelerationStructureModeKHR::eCompact;
        copy_cmd.copyAccelerationStructureKHR(copy_info);
    }
    bool const copy_submitted = Kataglyphis::VulkanRendererInternals::CommandBufferManager::endAndSubmitCommandBuffer(
      logical, commandPool, device->getGraphicsQueue(), copy_cmd);
    if (!copy_submitted) {
        spdlog::error("ASManager::compactBLAS: failed to submit BLAS copy commands; keeping uncompacted BLAS.");
        for (uint32_t i = 0; i < count; ++i) {
            logical.destroyAccelerationStructureKHR(compacted[i].vulkanAS);
            compacted[i].vulkanBuffer.cleanUp();
        }
        logical.destroyQueryPool(query_pool);
        return;
    }

    // 3. The submit was synchronous, so the originals can go; the TLAS is built later from the new addresses.
    for (uint32_t i = 0; i < count; ++i) {
        logical.destroyAccelerationStructureKHR(blas[i].vulkanAS);
        blas[i].vulkanBuffer.cleanUp();
    }
    blas = std::move(compacted);
    logical.destroyQueryPool(query_pool);

    spdlog::info("ASManager: compacted {} BLAS, {} bytes total after compaction.", count, total_compacted);
}

void Kataglyphis::VulkanRendererInternals::ASManager::createTLAS(const std::shared_ptr<VulkanDevice> &device,
  vk::CommandPool commandPool,
  Kataglyphis::Scene *scene)
{
    if (blas.size() < scene->getModelCount()) {
        spdlog::error(
          "ASManager::createTLAS: fewer BLAS ({}) than models ({}); a prior BLAS build failed, skipping TLAS "
          "build and keeping the previous TLAS.",
          blas.size(),
          scene->getModelCount());
        return;
    }

    // Clear old TLAS
    if (tlas.vulkanAS) {
        device->getLogicalDevice().destroyAccelerationStructureKHR(tlas.vulkanAS);
        tlas.vulkanAS = nullptr;
    }
    tlas.vulkanBuffer.cleanUp();

    std::vector<vk::AccelerationStructureInstanceKHR> tlas_instances;
    tlas_instances.reserve(scene->getModelCount());

    // instanceCustomIndex is the flat index of the model's first mesh, not model_index; shaders add GeometryIndex().
    const std::vector<uint32_t> mesh_base_offsets = Kataglyphis::meshBaseOffsets(scene->getMeshCountPerModel());
    for (size_t model_index = 0; model_index < scene->getModelCount(); model_index++) {
        glm::mat4 transpose_transform = glm::transpose(scene->getModelMatrix(static_cast<uint32_t>(model_index)));
        vk::TransformMatrixKHR out_matrix;
        memcpy(&out_matrix, &transpose_transform, sizeof(vk::TransformMatrixKHR));

        vk::AccelerationStructureDeviceAddressInfoKHR acceleration_structure_device_address_info{};
        acceleration_structure_device_address_info.accelerationStructure = blas[model_index].vulkanAS;

        vk::DeviceAddress const acceleration_structure_device_address =
          device->getLogicalDevice().getAccelerationStructureAddressKHR(acceleration_structure_device_address_info);

        vk::AccelerationStructureInstanceKHR geometry_instance{};
        geometry_instance.transform = out_matrix;
        geometry_instance.instanceCustomIndex = mesh_base_offsets[model_index];
        geometry_instance.mask = 0xFF;
        geometry_instance.instanceShaderBindingTableRecordOffset = 0;
        geometry_instance.flags =
          static_cast<VkGeometryInstanceFlagsKHR>(vk::GeometryInstanceFlagBitsKHR::eTriangleFacingCullDisable);
        geometry_instance.accelerationStructureReference = acceleration_structure_device_address;

        tlas_instances.emplace_back(geometry_instance);
    }

    vk::CommandBuffer command_buffer = Kataglyphis::VulkanRendererInternals::CommandBufferManager::beginCommandBuffer(
      device->getLogicalDevice(), commandPool);
    if (!command_buffer) {
        spdlog::error("ASManager::createTLAS: failed to begin command buffer, skipping TLAS build.");
        return;
    }

    VulkanBuffer geometryInstanceBuffer;

    if (!vulkanBufferManager.createBufferAndUploadVectorOnDevice(device,
          commandPool,
          geometryInstanceBuffer,
          vk::BufferUsageFlagBits::eShaderDeviceAddress
            | vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR
            | vk::BufferUsageFlagBits::eTransferDst,
          vk::MemoryPropertyFlagBits::eDeviceLocal,
          tlas_instances,
          vk::MemoryAllocateFlagBits::eDeviceAddress)) {
        spdlog::error("ASManager::createTLAS: geometry instance buffer upload failed, skipping TLAS build.");
        return;
    }

    vk::BufferDeviceAddressInfo geometry_instance_buffer_device_address_info{};
    geometry_instance_buffer_device_address_info.buffer = geometryInstanceBuffer.getBuffer();

    vk::DeviceAddress const geometry_instance_buffer_address =
      VULKAN_HPP_DEFAULT_DISPATCHER.vkGetBufferDeviceAddress(static_cast<VkDevice>(device->getLogicalDevice()),
        reinterpret_cast<VkBufferDeviceAddressInfo *>(&geometry_instance_buffer_device_address_info));

    vk::MemoryBarrier barrier;
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eAccelerationStructureWriteKHR;
    command_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
      vk::PipelineStageFlagBits::eAccelerationStructureBuildKHR,
      {},
      barrier,
      {},
      {});

    vk::AccelerationStructureGeometryInstancesDataKHR acceleration_structure_geometry_instances_data{};
    acceleration_structure_geometry_instances_data.data.deviceAddress = geometry_instance_buffer_address;

    vk::AccelerationStructureGeometryKHR topAS_acceleration_structure_geometry{};
    topAS_acceleration_structure_geometry.geometryType = vk::GeometryTypeKHR::eInstances;
    topAS_acceleration_structure_geometry.geometry.instances = acceleration_structure_geometry_instances_data;

    vk::AccelerationStructureBuildGeometryInfoKHR acceleration_structure_build_geometry_info{};
    acceleration_structure_build_geometry_info.type = vk::AccelerationStructureTypeKHR::eTopLevel;
    acceleration_structure_build_geometry_info.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
    acceleration_structure_build_geometry_info.mode = vk::BuildAccelerationStructureModeKHR::eBuild;
    acceleration_structure_build_geometry_info.srcAccelerationStructure = nullptr;
    acceleration_structure_build_geometry_info.geometryCount = 1;
    acceleration_structure_build_geometry_info.pGeometries = &topAS_acceleration_structure_geometry;

    vk::AccelerationStructureBuildSizesInfoKHR acceleration_structure_build_sizes_info{};

    auto const count_instance = static_cast<uint32_t>(tlas_instances.size());
    device->getLogicalDevice().getAccelerationStructureBuildSizesKHR(vk::AccelerationStructureBuildTypeKHR::eDevice,
      &acceleration_structure_build_geometry_info,
      &count_instance,
      &acceleration_structure_build_sizes_info);

    VulkanBuffer &tlasVulkanBuffer = tlas.vulkanBuffer;
    tlasVulkanBuffer.create(device,
      acceleration_structure_build_sizes_info.accelerationStructureSize,
      vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress
        | vk::BufferUsageFlagBits::eTransferDst,
      vk::MemoryPropertyFlagBits::eDeviceLocal,
      vk::MemoryAllocateFlagBits::eDeviceAddress);

    vk::AccelerationStructureCreateInfoKHR acceleration_structure_create_info{};
    acceleration_structure_create_info.createFlags = {};
    acceleration_structure_create_info.buffer = tlasVulkanBuffer.getBuffer();
    acceleration_structure_create_info.offset = 0;
    acceleration_structure_create_info.size = acceleration_structure_build_sizes_info.accelerationStructureSize;
    acceleration_structure_create_info.type = vk::AccelerationStructureTypeKHR::eTopLevel;
    acceleration_structure_create_info.deviceAddress = 0;

    vk::AccelerationStructureKHR &tlAS = tlas.vulkanAS;
    vk::ResultValue<vk::AccelerationStructureKHR> tlas_result =
      device->getLogicalDevice().createAccelerationStructureKHR(acceleration_structure_create_info);
    ASSERT_VULKAN(tlas_result.result, "Failed to create top-level acceleration structure!");
    tlAS = tlas_result.value;

    VulkanBuffer scratchBuffer;

    scratchBuffer.create(device,
      acceleration_structure_build_sizes_info.buildScratchSize,
      vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress
        | vk::BufferUsageFlagBits::eTransferDst,
      vk::MemoryPropertyFlagBits::eDeviceLocal,
      vk::MemoryAllocateFlagBits::eDeviceAddress);

    vk::BufferDeviceAddressInfo scratch_buffer_device_address_info{};
    scratch_buffer_device_address_info.buffer = scratchBuffer.getBuffer();

    vk::DeviceAddress const scratch_buffer_address =
      VULKAN_HPP_DEFAULT_DISPATCHER.vkGetBufferDeviceAddress(static_cast<VkDevice>(device->getLogicalDevice()),
        reinterpret_cast<VkBufferDeviceAddressInfo *>(&scratch_buffer_device_address_info));

    acceleration_structure_build_geometry_info.scratchData.deviceAddress = scratch_buffer_address;
    acceleration_structure_build_geometry_info.srcAccelerationStructure = nullptr;
    acceleration_structure_build_geometry_info.dstAccelerationStructure = tlAS;

    vk::AccelerationStructureBuildRangeInfoKHR acceleration_structure_build_range_info{};
    acceleration_structure_build_range_info.primitiveCount = scene->getModelCount();
    acceleration_structure_build_range_info.primitiveOffset = 0;
    acceleration_structure_build_range_info.firstVertex = 0;
    acceleration_structure_build_range_info.transformOffset = 0;

    vk::AccelerationStructureBuildRangeInfoKHR const *acceleration_structure_build_range_infos =
      &acceleration_structure_build_range_info;

    command_buffer.buildAccelerationStructuresKHR(
      1, &acceleration_structure_build_geometry_info, &acceleration_structure_build_range_infos);

    bool const build_submitted = Kataglyphis::VulkanRendererInternals::CommandBufferManager::endAndSubmitCommandBuffer(
      device->getLogicalDevice(), commandPool, device->getGraphicsQueue(), command_buffer);
    if (!build_submitted) {
        // The only signal: the TLAS handle exists but was never built, and the next traced frame reads it.
        spdlog::error("ASManager::createTLAS: failed to submit TLAS build commands; the top-level "
                      "acceleration structure handle exists but was never built.");
    }
    scratchBuffer.cleanUp();
    geometryInstanceBuffer.cleanUp();
}

void Kataglyphis::VulkanRendererInternals::ASManager::cleanUp()
{
    // Normal when nothing was built (model still parsing, or no ray tracing): only createASForScene sets the device.
    if (!vulkanDevice) { return; }

    // Release the reusable staging buffer while the VMA allocator is alive.
    vulkanBufferManager.cleanUp();

    vulkanDevice->getLogicalDevice().destroyAccelerationStructureKHR(tlas.vulkanAS);
    tlas.vulkanAS = nullptr;

    tlas.vulkanBuffer.cleanUp();

    for (auto &bla : blas) {
        vulkanDevice->getLogicalDevice().destroyAccelerationStructureKHR(bla.vulkanAS);
        bla.vulkanAS = nullptr;

        bla.vulkanBuffer.cleanUp();
    }
    blas.clear();

    vulkanDevice.reset();
}

Kataglyphis::VulkanRendererInternals::ASManager::~ASManager() { cleanUp(); }

void Kataglyphis::VulkanRendererInternals::ASManager::createSingleBlas(const std::shared_ptr<VulkanDevice> &device,
  vk::CommandBuffer command_buffer,
  BuildAccelerationStructure &build_as_structure,
  vk::DeviceAddress scratch_device_or_host_address)
{
    vk::AccelerationStructureCreateInfoKHR acceleration_structure_create_info{};
    acceleration_structure_create_info.type = vk::AccelerationStructureTypeKHR::eBottomLevel;
    acceleration_structure_create_info.size = build_as_structure.size_info.accelerationStructureSize;
    VulkanBuffer &blasVulkanBuffer = build_as_structure.single_blas.vulkanBuffer;
    blasVulkanBuffer.create(device,
      build_as_structure.size_info.accelerationStructureSize,
      vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress
        | vk::BufferUsageFlagBits::eTransferDst,
      vk::MemoryPropertyFlagBits::eDeviceLocal,
      vk::MemoryAllocateFlagBits::eDeviceAddress);

    acceleration_structure_create_info.buffer = blasVulkanBuffer.getBuffer();
    vk::AccelerationStructureKHR &blas_as = build_as_structure.single_blas.vulkanAS;
    vk::ResultValue<vk::AccelerationStructureKHR> blas_result =
      device->getLogicalDevice().createAccelerationStructureKHR(acceleration_structure_create_info);
    ASSERT_VULKAN(blas_result.result, "Failed to create bottom-level acceleration structure!");
    blas_as = blas_result.value;

    build_as_structure.build_info.dstAccelerationStructure = blas_as;
    build_as_structure.build_info.scratchData.deviceAddress = scratch_device_or_host_address;

    command_buffer.buildAccelerationStructuresKHR(1, &build_as_structure.build_info, &build_as_structure.range_info);
}

void Kataglyphis::VulkanRendererInternals::ASManager::createAccelerationStructureInfosBLAS(const std::shared_ptr<VulkanDevice> &device,
  BuildAccelerationStructure &build_as_structure,
  BlasInput &blas_input,
  vk::DeviceSize &current_scratch_size,
  vk::DeviceSize &current_size)
{
    build_as_structure.build_info.type = vk::AccelerationStructureTypeKHR::eBottomLevel;
    // eAllowCompaction enables compactBLAS without slowing the trace.
    build_as_structure.build_info.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace
      | vk::BuildAccelerationStructureFlagBitsKHR::eAllowCompaction;
    build_as_structure.build_info.mode = vk::BuildAccelerationStructureModeKHR::eBuild;
    build_as_structure.build_info.geometryCount = static_cast<uint32_t>(blas_input.as_geometry.size());
    build_as_structure.build_info.pGeometries = blas_input.as_geometry.data();

    build_as_structure.range_info = blas_input.as_build_offset_info.data();

    std::vector<uint32_t> max_primitive_cnt(blas_input.as_build_offset_info.size());

    for (uint32_t temp = 0; temp < static_cast<uint32_t>(blas_input.as_build_offset_info.size()); temp++) {
        max_primitive_cnt[temp] = blas_input.as_build_offset_info[temp].primitiveCount;
    }

    device->getLogicalDevice().getAccelerationStructureBuildSizesKHR(vk::AccelerationStructureBuildTypeKHR::eDevice,
      &build_as_structure.build_info,
      max_primitive_cnt.data(),
      &build_as_structure.size_info);

    current_size = build_as_structure.size_info.accelerationStructureSize;
    current_scratch_size = build_as_structure.size_info.buildScratchSize;
}

void Kataglyphis::VulkanRendererInternals::ASManager::objectToVkGeometryKHR(const std::shared_ptr<VulkanDevice> &device,
  Kataglyphis::Mesh *mesh,
  vk::AccelerationStructureGeometryKHR &acceleration_structure_geometry,
  vk::AccelerationStructureBuildRangeInfoKHR &acceleration_structure_build_range_info)
{
    vk::BufferDeviceAddressInfo vertex_buffer_device_address_info{};
    vertex_buffer_device_address_info.buffer = mesh->getVertexBuffer();

    vk::BufferDeviceAddressInfo index_buffer_device_address_info{};
    index_buffer_device_address_info.buffer = mesh->getIndexBuffer();

    vk::DeviceAddress const vertex_buffer_address =
      VULKAN_HPP_DEFAULT_DISPATCHER.vkGetBufferDeviceAddress(static_cast<VkDevice>(device->getLogicalDevice()),
        reinterpret_cast<VkBufferDeviceAddressInfo *>(&vertex_buffer_device_address_info));
    vk::DeviceAddress const index_buffer_address =
      VULKAN_HPP_DEFAULT_DISPATCHER.vkGetBufferDeviceAddress(static_cast<VkDevice>(device->getLogicalDevice()),
        reinterpret_cast<VkBufferDeviceAddressInfo *>(&index_buffer_device_address_info));

    vk::DeviceOrHostAddressConstKHR vertex_device_or_host_address_const{};
    vertex_device_or_host_address_const.deviceAddress = vertex_buffer_address;

    vk::DeviceOrHostAddressConstKHR index_device_or_host_address_const{};
    index_device_or_host_address_const.deviceAddress = index_buffer_address;

    vk::AccelerationStructureGeometryTrianglesDataKHR acceleration_structure_triangles_data{};
    acceleration_structure_triangles_data.vertexFormat = vk::Format::eR32G32B32Sfloat;
    acceleration_structure_triangles_data.vertexData = vertex_device_or_host_address_const;
    acceleration_structure_triangles_data.vertexStride = sizeof(Vertex);
    const auto limits = Kataglyphis::blasTriangleLimits(mesh->getVertexCount(), mesh->getIndexCount());
    acceleration_structure_triangles_data.maxVertex = limits.maxVertex;
    acceleration_structure_triangles_data.indexType = vk::IndexType::eUint32;
    acceleration_structure_triangles_data.indexData = index_device_or_host_address_const;

    vk::AccelerationStructureGeometryDataKHR acceleration_structure_geometry_data{};
    acceleration_structure_geometry_data.triangles = acceleration_structure_triangles_data;

    acceleration_structure_geometry.geometryType = vk::GeometryTypeKHR::eTriangles;
    acceleration_structure_geometry.geometry = acceleration_structure_geometry_data;
    // eOpaque unless a MASK material needs raytrace.rahit.slang's any-hit alpha test.
    acceleration_structure_geometry.flags = Kataglyphis::blasGeometryFlags(mesh->hasMaskedMaterial());

    acceleration_structure_build_range_info.primitiveCount = limits.primitiveCount;
    acceleration_structure_build_range_info.primitiveOffset = 0;
    acceleration_structure_build_range_info.firstVertex = 0;
    acceleration_structure_build_range_info.transformOffset = 0;
}