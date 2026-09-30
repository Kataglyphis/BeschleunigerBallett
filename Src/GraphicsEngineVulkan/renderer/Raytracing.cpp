module;
#include <memory>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <sstream>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "common/ImageBarrierHelper.hpp"
#include "common/MemoryHelper.hpp"
#include "common/PipelineLayoutHelper.hpp"
#include "common/ShaderStageHelper.hpp"
#include "common/Utilities.hpp"
#include <spdlog/spdlog.h>
#include "renderer/pushConstants/PushConstantRayTracing.hpp"

module kataglyphis.vulkan.raytracing;

import kataglyphis.vulkan.shader_helper;
import kataglyphis.vulkan.swapchain;

Kataglyphis::VulkanRendererInternals::Raytracing::Raytracing() = default;

void Kataglyphis::VulkanRendererInternals::Raytracing::init(const std::shared_ptr<VulkanDevice> &in_device,
  std::span<const vk::DescriptorSetLayout> descriptorSetLayouts,
  VulkanSwapChain *swapchain)
{
    this->device = in_device;
    this->vulkanSwapChain = swapchain;

    createPCRange();
    createGraphicsPipeline(descriptorSetLayouts);
    createSBT();
}

void Kataglyphis::VulkanRendererInternals::Raytracing::shaderHotReload(
  std::span<const vk::DescriptorSetLayout> descriptor_set_layouts)
{
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), graphicsPipeline, pipeline_layout);
    createGraphicsPipeline(descriptor_set_layouts);
    recreateSBT();
}

void Kataglyphis::VulkanRendererInternals::Raytracing::recordCommands(vk::CommandBuffer &commandBuffer,
  VulkanImage &renderImage,
  std::span<const vk::DescriptorSet> descriptorSets)
{
    uint32_t const handle_size = raytracing_properties.shaderGroupHandleSize;
    uint32_t const handle_size_aligned = align_up(handle_size, raytracing_properties.shaderGroupHandleAlignment);

    vk::Device logical_device = device->getLogicalDevice();

    vk::BufferDeviceAddressInfo bufferDeviceAI{};
    bufferDeviceAI.buffer = raygenShaderBindingTableBuffer.getBuffer();

    rgen_region.deviceAddress = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetBufferDeviceAddress(
      static_cast<VkDevice>(logical_device), reinterpret_cast<VkBufferDeviceAddressInfo *>(&bufferDeviceAI));
    rgen_region.stride = handle_size_aligned;
    rgen_region.size = handle_size_aligned;

    bufferDeviceAI.buffer = missShaderBindingTableBuffer.getBuffer();
    miss_region.deviceAddress = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetBufferDeviceAddress(
      static_cast<VkDevice>(logical_device), reinterpret_cast<VkBufferDeviceAddressInfo *>(&bufferDeviceAI));
    miss_region.stride = handle_size_aligned;
    // Two miss records: 0 primary, 1 shadow (raytrace.rchit.slang's shadow TraceRay).
    miss_region.size = 2 * handle_size_aligned;

    bufferDeviceAI.buffer = hitShaderBindingTableBuffer.getBuffer();
    hit_region.deviceAddress = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetBufferDeviceAddress(
      static_cast<VkDevice>(logical_device), reinterpret_cast<VkBufferDeviceAddressInfo *>(&bufferDeviceAI));
    hit_region.stride = handle_size_aligned;
    hit_region.size = handle_size_aligned;

    pc.clear_color = { 0.0F, 0.0F, 0.0F, 0.0F };
    commandBuffer.pushConstants(pipeline_layout,
      vk::ShaderStageFlagBits::eRaygenKHR | vk::ShaderStageFlagBits::eClosestHitKHR | vk::ShaderStageFlagBits::eMissKHR,
      0,
      sizeof(PushConstantRaytracing),
      reinterpret_cast<void *>(&pc));

    commandBuffer.bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, graphicsPipeline);

    // eUndefined: rgen overwrites every pixel, whether or not the raster pass ran this frame.
    const vk::ImageMemoryBarrier rasterizerToRaytracingImageBarrier = Kataglyphis::buildImageMemoryBarrier(
      renderImage.getImage(), vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral, {}, vk::AccessFlagBits::eShaderWrite);

    commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
      vk::PipelineStageFlagBits::eRayTracingShaderKHR,
      {},
      {},
      {},
      rasterizerToRaytracingImageBarrier);

    commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eRayTracingKHR, pipeline_layout, 0, descriptorSets, {});

    if (!this->vulkanSwapChain) {
      spdlog::error("Raytracing::recordCommands: VulkanSwapChain is null");
      return;
    }
    const vk::Extent2D &swap_chain_extent = this->vulkanSwapChain->getSwapChainExtent();
    commandBuffer.traceRaysKHR(
      rgen_region, miss_region, hit_region, call_region, swap_chain_extent.width, swap_chain_extent.height, 1);

    const vk::ImageMemoryBarrier raytracingToPostImageBarrier =
      Kataglyphis::buildImageMemoryBarrier(renderImage.getImage(),
        vk::ImageLayout::eGeneral,
        vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::AccessFlagBits::eShaderWrite,
        vk::AccessFlagBits::eShaderRead);

    commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eRayTracingShaderKHR,
      vk::PipelineStageFlagBits::eFragmentShader,
      {},
      {},
      {},
      raytracingToPostImageBarrier);
}

void Kataglyphis::VulkanRendererInternals::Raytracing::cleanUp()
{
    // Idempotent, and a no-op when init() never ran (no hardware ray tracing).
    if (!device) { return; }

    shaderBindingTableBuffer.cleanUp();
    raygenShaderBindingTableBuffer.cleanUp();
    missShaderBindingTableBuffer.cleanUp();
    hitShaderBindingTableBuffer.cleanUp();

    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), graphicsPipeline, pipeline_layout);

    device.reset();
}

Kataglyphis::VulkanRendererInternals::Raytracing::~Raytracing() { cleanUp(); }

void Kataglyphis::VulkanRendererInternals::Raytracing::createPCRange()
{
    pc_ranges.stageFlags =
      vk::ShaderStageFlagBits::eRaygenKHR | vk::ShaderStageFlagBits::eClosestHitKHR | vk::ShaderStageFlagBits::eMissKHR;
    pc_ranges.offset = 0;
    pc_ranges.size = sizeof(PushConstantRaytracing);
}

void Kataglyphis::VulkanRendererInternals::Raytracing::createGraphicsPipeline(
  std::span<const vk::DescriptorSetLayout> descriptorSetLayouts)
{
    // Relative path: the engine runs from the repo root.
    std::string const slang_spv_dir = "Resources/ShadersSlang/build/spirv/raytracing/";

    std::string const raygen_spv = "raytrace.rgen.rgen_main.spv";
    std::string const chit_spv = "raytrace.rchit.rchit_main.spv";
    std::string const ahit_spv = "raytrace.rahit.rahit_main.spv";
    std::string const miss_spv = "raytrace.rmiss.rmiss_main.spv";
    std::string const shadow_spv = "shadow.rmiss.shadow_rmiss_main.spv";

    vk::ShaderModule raygen_shader_module = loadSpirvShaderModule(device, slang_spv_dir + raygen_spv);
    vk::ShaderModule raychit_shader_module = loadSpirvShaderModule(device, slang_spv_dir + chit_spv);
    vk::ShaderModule rayahit_shader_module = loadSpirvShaderModule(device, slang_spv_dir + ahit_spv);
    vk::ShaderModule raymiss_shader_module = loadSpirvShaderModule(device, slang_spv_dir + miss_spv);
    vk::ShaderModule shadow_shader_module = loadSpirvShaderModule(device, slang_spv_dir + shadow_spv);

    std::array<vk::PipelineShaderStageCreateInfo, 5> shader_stages = {
        buildShaderStageCreateInfo(vk::ShaderStageFlagBits::eRaygenKHR, raygen_shader_module),
        buildShaderStageCreateInfo(vk::ShaderStageFlagBits::eMissKHR, raymiss_shader_module),
        buildShaderStageCreateInfo(vk::ShaderStageFlagBits::eMissKHR, shadow_shader_module),
        buildShaderStageCreateInfo(vk::ShaderStageFlagBits::eClosestHitKHR, raychit_shader_module),
        buildShaderStageCreateInfo(vk::ShaderStageFlagBits::eAnyHitKHR, rayahit_shader_module)
    };

    enum StageIndices { eRaygen, eMiss, eMiss2, eClosestHit, eAnyHit, eShaderGroupCount };

    shader_groups.reserve(4);

    shader_groups.push_back(buildGeneralShaderGroup(eRaygen));
    shader_groups.push_back(buildGeneralShaderGroup(eMiss));
    shader_groups.push_back(buildGeneralShaderGroup(eMiss2));
    // Any-hit joins the triangles-hit group, keeping the SBT hit region at one record.
    shader_groups.push_back(buildTrianglesHitGroup(eClosestHit, eAnyHit));

    const std::array<vk::PushConstantRange, 1> push_constant_ranges = { pc_ranges };
    vk::PipelineLayoutCreateInfo pipeline_layout_create_info =
      buildPipelineLayoutCreateInfo(descriptorSetLayouts, push_constant_ranges);

    vk::ResultValue<vk::PipelineLayout> pipeline_layout_result =
      device->getLogicalDevice().createPipelineLayout(pipeline_layout_create_info);
    ASSERT_VULKAN(pipeline_layout_result.result, "Failed to create raytracing pipeline layout!");
    pipeline_layout = pipeline_layout_result.value;

    vk::PipelineLibraryCreateInfoKHR pipeline_library_create_info{};
    pipeline_library_create_info.libraryCount = 0;
    pipeline_library_create_info.pLibraries = nullptr;

    vk::RayTracingPipelineCreateInfoKHR raytracing_pipeline_create_info{};
    raytracing_pipeline_create_info.flags = {};
    raytracing_pipeline_create_info.stageCount = static_cast<uint32_t>(shader_stages.size());
    raytracing_pipeline_create_info.pStages = shader_stages.data();
    raytracing_pipeline_create_info.groupCount = static_cast<uint32_t>(shader_groups.size());
    raytracing_pipeline_create_info.pGroups = shader_groups.data();
    raytracing_pipeline_create_info.maxPipelineRayRecursionDepth = 2;
    raytracing_pipeline_create_info.layout = pipeline_layout;

    vk::Result result2 = device->getLogicalDevice().createRayTracingPipelinesKHR(nullptr,
      device->getPipelineCache(),
      1,
      &raytracing_pipeline_create_info,
      nullptr,
      &graphicsPipeline,
      VULKAN_HPP_DEFAULT_DISPATCHER);
    ASSERT_VULKAN(result2, "Failed to create raytracing pipeline!");

    device->getLogicalDevice().destroyShaderModule(raygen_shader_module);
    device->getLogicalDevice().destroyShaderModule(raymiss_shader_module);
    device->getLogicalDevice().destroyShaderModule(raychit_shader_module);
    device->getLogicalDevice().destroyShaderModule(rayahit_shader_module);
    device->getLogicalDevice().destroyShaderModule(shadow_shader_module);
}

void Kataglyphis::VulkanRendererInternals::Raytracing::createSBT()
{
    raytracing_properties = vk::PhysicalDeviceRayTracingPipelinePropertiesKHR{};

    vk::PhysicalDeviceProperties2 properties{};
    properties.pNext = &raytracing_properties;

    device->getPhysicalDevice().getProperties2(&properties);

    uint32_t const handle_size = raytracing_properties.shaderGroupHandleSize;
    uint32_t const handle_size_aligned = align_up(handle_size, raytracing_properties.shaderGroupHandleAlignment);

    auto const group_count = static_cast<uint32_t>(shader_groups.size());
    uint32_t const sbt_size = group_count * handle_size_aligned;

    std::vector<uint8_t> handles(sbt_size);

    vk::Result result = device->getLogicalDevice().getRayTracingShaderGroupHandlesKHR(
      graphicsPipeline, 0, group_count, sbt_size, handles.data(), VULKAN_HPP_DEFAULT_DISPATCHER);
    ASSERT_VULKAN(result, "Failed to get ray tracing shader group handles!");

    const vk::BufferUsageFlags bufferUsageFlags =
      vk::BufferUsageFlagBits::eShaderBindingTableKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress;
    const vk::MemoryPropertyFlags memoryPropertyFlags =
      vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    const vk::MemoryAllocateFlags memoryAllocateFlags = vk::MemoryAllocateFlagBits::eDeviceAddress;

    // Handles arrive packed at handle_size but records start at handle_size_aligned; the two can differ.
    raygenShaderBindingTableBuffer.create(
      device, handle_size_aligned, bufferUsageFlags, memoryPropertyFlags, memoryAllocateFlags);

    missShaderBindingTableBuffer.create(
      device, 2 * handle_size_aligned, bufferUsageFlags, memoryPropertyFlags, memoryAllocateFlags);

    hitShaderBindingTableBuffer.create(
      device, handle_size_aligned, bufferUsageFlags, memoryPropertyFlags, memoryAllocateFlags);

    // SBT buffers are host-visible and therefore persistently mapped by VMA.
    void *mapped_raygen = raygenShaderBindingTableBuffer.getMappedData();

    void *mapped_miss = missShaderBindingTableBuffer.getMappedData();

    void *mapped_rchit = hitShaderBindingTableBuffer.getMappedData();

    // Group layout: 0 raygen, 1 miss, 2 shadow miss, 3 triangles-hit.
    memcpy(mapped_raygen,
      handles.data() + sbt_handle_source_offset(0, handle_size),
      handle_size);
    memcpy(static_cast<uint8_t *>(mapped_miss) + sbt_record_offset(0, handle_size_aligned),
      handles.data() + sbt_handle_source_offset(1, handle_size),
      handle_size);
    memcpy(static_cast<uint8_t *>(mapped_miss) + sbt_record_offset(1, handle_size_aligned),
      handles.data() + sbt_handle_source_offset(2, handle_size),
      handle_size);
    memcpy(mapped_rchit,
      handles.data() + sbt_handle_source_offset(3, handle_size),
      handle_size);
}

void Kataglyphis::VulkanRendererInternals::Raytracing::recreateSBT()
{
    // Handles belong to the old pipeline, and VulkanBuffer::create does not free a prior allocation.
    raygenShaderBindingTableBuffer.cleanUp();
    missShaderBindingTableBuffer.cleanUp();
    hitShaderBindingTableBuffer.cleanUp();

    createSBT();
}