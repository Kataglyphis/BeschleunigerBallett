#include "kompute/Algorithm.hpp"
#include "kompute/Manager.hpp"
#include "kompute/Memory.hpp"
#include "kompute/Tensor.hpp"
#include "kompute/operations/OpAlgoDispatch.hpp"
#include "kompute/operations/OpMemoryBarrier.hpp"
#include "kompute/operations/OpSyncDevice.hpp"
#include "kompute/operations/OpSyncLocal.hpp"
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <vector>

// glslangValidator --vn output: a bare uint32_t array that relies on <cstdint> above.
#include <shader/my_shader.hpp>

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL countValidationMessage(VkDebugReportFlagsEXT /*flags*/,
  VkDebugReportObjectTypeEXT /*objectType*/,
  uint64_t /*object*/,
  size_t /*location*/,
  int32_t /*messageCode*/,
  const char *layerPrefix,
  const char *message,
  void *messages)
{
    ++*static_cast<int *>(messages);
    std::cerr << "[VALIDATION] " << layerPrefix << ": " << message << '\n';
    return VK_FALSE;
}

}// namespace

auto main() -> int
{
    kp::Manager mgr;

    // Kompute's own callback logs a message at debug level without its id, so no log scan sees a hazard: count them.
    const auto instance = static_cast<VkInstance>(*mgr.getVkInstance());
    const auto createCallback = reinterpret_cast<PFN_vkCreateDebugReportCallbackEXT>(
      vkGetInstanceProcAddr(instance, "vkCreateDebugReportCallbackEXT"));
    const auto destroyCallback = reinterpret_cast<PFN_vkDestroyDebugReportCallbackEXT>(
      vkGetInstanceProcAddr(instance, "vkDestroyDebugReportCallbackEXT"));
    int validationMessages = 0;
    VkDebugReportCallbackCreateInfoEXT callbackInfo{};
    callbackInfo.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
    callbackInfo.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT;
    callbackInfo.pfnCallback = countValidationMessage;
    callbackInfo.pUserData = &validationMessages;
    VkDebugReportCallbackEXT callback = VK_NULL_HANDLE;
    if (createCallback == nullptr || destroyCallback == nullptr
        || createCallback(instance, &callbackInfo, nullptr, &callback) != VK_SUCCESS) {
        throw std::runtime_error("Could not register a VK_EXT_debug_report callback");
    }

    std::shared_ptr<kp::TensorT<float>> const tensorInA = mgr.tensor({ 2.0, 4.0, 6.0 });
    std::shared_ptr<kp::TensorT<float>> const tensorInB = mgr.tensor({ 0.0, 1.0, 2.0 });
    std::shared_ptr<kp::TensorT<float>> const tensorOut = mgr.tensor({ 0.0, 0.0, 0.0 });

    const std::vector<std::shared_ptr<kp::Memory>> params = { tensorInA, tensorInB, tensorOut };

    const std::vector<uint32_t> shader(std::begin(MY_SHADER_COMP_SPV), std::end(MY_SHADER_COMP_SPV));
    std::shared_ptr<kp::Algorithm> const algo = mgr.algorithm(params, shader);

    // OpAlgoDispatch's own barrier makes the copies visible to shader reads only, so a storage write needs this one.
    mgr.sequence()
      ->record<kp::OpSyncDevice>(params)
      ->record<kp::OpMemoryBarrier>(params,
        vk::AccessFlagBits::eTransferWrite,
        vk::AccessFlagBits::eShaderWrite,
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eComputeShader)
      ->record<kp::OpAlgoDispatch>(algo)
      ->record<kp::OpSyncLocal>(params)
      ->eval();
    destroyCallback(instance, callback, nullptr);

    // prints "Output {  0  4  12  }"
    std::cout << "Output: {  ";
    for (const float &elem : tensorOut->vector()) { std::cout << elem << "  "; }
    std::cout << "}" << '\n';

    if (tensorOut->vector() != std::vector<float>{ 0, 4, 12 }) { throw std::runtime_error("Result does not match"); }
    if (validationMessages != 0) {
        std::cerr << validationMessages << " validation message(s); see the [VALIDATION] lines above\n";
        return 1;
    }
}
