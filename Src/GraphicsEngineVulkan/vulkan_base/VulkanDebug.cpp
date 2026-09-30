module;

#include <array>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_structs.hpp>
#include "spdlog/spdlog.h"

#include "common/Utilities.hpp"

module kataglyphis.vulkan.debug;

namespace Kataglyphis::debug {
static vk::DebugUtilsMessengerEXT debugUtilsMessenger;

VKAPI_ATTR static VkBool32 VKAPI_CALL debugUtilsMessengerCallback(
  vk::DebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
  vk::DebugUtilsMessageTypeFlagsEXT /*messageType*/,
  const vk::DebugUtilsMessengerCallbackDataEXT *pCallbackData,
  void * /*pUserData*/)
{
    // Select prefix depending on flags passed to the callback
    std::string prefix;

    if ((messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose)
        == vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose) {
        prefix = "VERBOSE: ";
    } else if ((messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo)
               == vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo) {
        prefix = "INFO: ";
    } else if ((messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning)
               == vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning) {
        prefix = "WARNING: ";
    } else if ((messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError)
               == vk::DebugUtilsMessageSeverityFlagBitsEXT::eError) {
        prefix = "ERROR: ";
    }

    // Display message to default output (console/logcat)
    std::stringstream debugMessage;
    debugMessage << prefix << "[" << pCallbackData->messageIdNumber << "][" << pCallbackData->pMessageIdName
                 << "] : " << pCallbackData->pMessage;

#ifdef __ANDROID__
    if (messageSeverity >= vk::DebugUtilsMessageSeverityFlagBitsEXT::eError) {
        LOGE("%s", debugMessage.str().c_str());
    } else {
        LOGD("%s", debugMessage.str().c_str());
    }
#else
    if (messageSeverity >= vk::DebugUtilsMessageSeverityFlagBitsEXT::eError) {
        spdlog::error(debugMessage.str());
    } else if (messageSeverity >= vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning) {
        spdlog::warn(debugMessage.str());
    } else if (messageSeverity >= vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo) {
        spdlog::info(debugMessage.str());
    } else {
        spdlog::debug(debugMessage.str());
    }
#endif

    // VK_FALSE: a validation message must not abort the Vulkan call that caused it.
    return VK_FALSE;
}

void setupDebugging(vk::Instance instance, vk::DebugReportFlagsEXT /*flags*/, vk::DebugReportCallbackEXT /*callBack*/)
{
    vk::DebugUtilsMessengerCreateInfoEXT debugUtilsMessengerCI{};
    debugUtilsMessengerCI.messageSeverity =
      vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning | vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
    debugUtilsMessengerCI.messageType =
      vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation;
    debugUtilsMessengerCI.pfnUserCallback = debugUtilsMessengerCallback;

    vk::Result result;
    std::tie(result, debugUtilsMessenger) = instance.createDebugUtilsMessengerEXT(debugUtilsMessengerCI, nullptr);
    ASSERT_VULKAN(result, "Failed to create debug messenger");
}

void freeDebugCallback(vk::Instance instance)
{
    if (debugUtilsMessenger) { instance.destroyDebugUtilsMessengerEXT(debugUtilsMessenger, nullptr); }
}

void beginCmdLabel(vk::CommandBuffer commandBuffer, const char *name, const std::array<float, 4> &color)
{
    // debug_utils is enabled only with the validation layers; otherwise the dispatcher pointer is null.
    if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdBeginDebugUtilsLabelEXT == nullptr) { return; }

    vk::DebugUtilsLabelEXT label{};
    label.pLabelName = name;
    label.color[0] = color[0];
    label.color[1] = color[1];
    label.color[2] = color[2];
    label.color[3] = color[3];
    commandBuffer.beginDebugUtilsLabelEXT(label);
}

void endCmdLabel(vk::CommandBuffer commandBuffer)
{
    if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdEndDebugUtilsLabelEXT == nullptr) { return; }
    commandBuffer.endDebugUtilsLabelEXT();
}
}// namespace Kataglyphis::debug
