module;
#include <memory>

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "common/ComputePipelineHelper.hpp"
#include "common/PipelineLayoutHelper.hpp"
#include "common/ShaderStageHelper.hpp"
#include "common/Utilities.hpp"
#include "spdlog/spdlog.h"

module kataglyphis.vulkan.shader_helper;

import kataglyphis.vulkan.device;
import kataglyphis.shared.util.file_reader;

namespace {

constexpr uint32_t kSpirvMagic = 0x07230203;

auto createShaderModuleFromBytes(const std::shared_ptr<Kataglyphis::VulkanDevice> &device, std::span<const char> code)
  -> vk::ShaderModule
{
    vk::ShaderModuleCreateInfo shader_module_create_info{};
    shader_module_create_info.codeSize = code.size();// size of code
    shader_module_create_info.pCode = reinterpret_cast<const uint32_t *>(code.data());// pointer to code

    auto shader_module_result = device->getLogicalDevice().createShaderModule(shader_module_create_info);
    if (shader_module_result.result != vk::Result::eSuccess) {
        spdlog::default_logger_raw()->log(spdlog::level::critical,
          std::string("Failed to create shader module (result ")
            + std::to_string(static_cast<int>(shader_module_result.result)) + ")");
        std::abort();
    }

    return shader_module_result.value;
}

}// namespace

auto Kataglyphis::validateSpirvBlob(std::span<const char> code) -> bool
{
    if (code.empty() || code.size() % sizeof(uint32_t) != 0) { return false; }

    uint32_t magic = 0;
    std::memcpy(&magic, code.data(), sizeof(magic));
    return magic == kSpirvMagic;
}

auto Kataglyphis::loadSpirvShaderModule(const std::shared_ptr<VulkanDevice> &device, const std::string &spvPath)
  -> vk::ShaderModule
{
    // No missing-file log: the check below logs critical with a better message and aborts.
    const std::vector<char> code = Kataglyphis::Shared::readBinaryFile(spvPath);

    if (!validateSpirvBlob(code)) {
        spdlog::default_logger_raw()->log(spdlog::level::critical,
          std::string("Invalid or missing SPIR-V shader blob at '") + spvPath
            + "' - run scripts/windows/Build-SlangShaders.ps1 (or the Linux .sh) to (re)compile shaders.");
        std::abort();
    }

    return createShaderModuleFromBytes(device, code);
}

Kataglyphis::ShaderStagePair::ShaderStagePair(std::shared_ptr<VulkanDevice> device,// DEVICE_SINK_OK: moved into member
  const std::string &vertexSpvPath,
  const std::string &fragmentSpvPath)
  : device_(std::move(device)), vertexModule_(loadSpirvShaderModule(device_, vertexSpvPath)),
    fragmentModule_(loadSpirvShaderModule(device_, fragmentSpvPath)),
    stages_{ buildShaderStageCreateInfo(vk::ShaderStageFlagBits::eVertex, vertexModule_),
        buildShaderStageCreateInfo(vk::ShaderStageFlagBits::eFragment, fragmentModule_) }
{}

Kataglyphis::ShaderStagePair::~ShaderStagePair()
{
    if (device_) {
        device_->getLogicalDevice().destroyShaderModule(vertexModule_);
        device_->getLogicalDevice().destroyShaderModule(fragmentModule_);
    }
}

auto Kataglyphis::createComputePipeline(const std::shared_ptr<VulkanDevice> &device,
  const std::string &spvPath,
  std::span<const vk::DescriptorSetLayout> setLayouts,
  std::span<const vk::PushConstantRange> pushConstantRanges,
  const char *layoutErrorMessage,
  const char *pipelineErrorMessage) -> ComputePipelineHandles
{
    vk::ShaderModule shaderModule = loadSpirvShaderModule(device, spvPath);

    const vk::PipelineShaderStageCreateInfo stageInfo = buildComputeShaderStageCreateInfo(shaderModule);

    const vk::PipelineLayoutCreateInfo pipelineLayoutInfo =
      buildPipelineLayoutCreateInfo(setLayouts, pushConstantRanges);

    auto layoutResult = device->getLogicalDevice().createPipelineLayout(pipelineLayoutInfo);
    ASSERT_VULKAN(layoutResult.result, layoutErrorMessage);
    ComputePipelineHandles handles{};
    handles.layout = layoutResult.value;

    const vk::ComputePipelineCreateInfo pipelineInfo = buildComputePipelineCreateInfo(stageInfo, handles.layout);

    auto pipelineResult = device->getLogicalDevice().createComputePipeline(device->getPipelineCache(), pipelineInfo);
    ASSERT_VULKAN(pipelineResult.result, pipelineErrorMessage);
    handles.pipeline = pipelineResult.value;

    device->getLogicalDevice().destroyShaderModule(shaderModule);

    return handles;
}
