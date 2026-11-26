module;
#include <memory>

#include <cstdint>
#include <cstring>
#include <filesystem>
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

import kataglyphis.vulkan.debug;
import kataglyphis.vulkan.device;
import kataglyphis.shared.util.file_reader;
import kataglyphis.shared.util.resource_paths;

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
    // The callers name paths below the install root; a relative one read from the cwd broke the 1.7.0 MSI.
    std::string path = spvPath;
    if (std::filesystem::path(spvPath).is_relative()) {
        if (const auto installed = Kataglyphis::Shared::resolveInstalledPath(spvPath); installed.has_value()) {
            path = installed->string();
        }
    }

    // No missing-file log: the check below logs critical with a better message and aborts.
    const std::vector<char> code = Kataglyphis::Shared::readBinaryFile(path);

    if (!validateSpirvBlob(code)) {
        spdlog::default_logger_raw()->log(spdlog::level::critical,
          std::string("Invalid or missing SPIR-V shader blob at '") + path
            + "' (searched beside the executable, then above the working directory) - run "
              "scripts/windows/Build-SlangShaders.ps1 (or the Linux .sh) to (re)compile shaders.");
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

    ComputePipelineHandles handles{};
    handles.layout = vkCheck(device->getLogicalDevice().createPipelineLayout(pipelineLayoutInfo), layoutErrorMessage);

    const vk::ComputePipelineCreateInfo pipelineInfo = buildComputePipelineCreateInfo(stageInfo, handles.layout);

    handles.pipeline = vkCheck(
      device->getLogicalDevice().createComputePipeline(device->getPipelineCache(), pipelineInfo), pipelineErrorMessage);
    // The SPIR-V file's stem, "clouds.clouds_main", is the entry point a capture shows.
    const std::size_t slash = spvPath.find_last_of("/\\");
    std::string name = spvPath.substr(slash == std::string::npos ? 0 : slash + 1);
    if (name.ends_with(".spv")) { name.resize(name.size() - 4); }
    debug::nameObject(device->getLogicalDevice(), handles.pipeline, name.c_str());

    device->getLogicalDevice().destroyShaderModule(shaderModule);

    return handles;
}
