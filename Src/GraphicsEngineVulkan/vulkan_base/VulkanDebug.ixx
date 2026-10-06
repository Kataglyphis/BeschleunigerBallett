module;

#include <array>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.debug;

export namespace Kataglyphis::debug {
extern int validationLayerCount;
extern const char *validationLayerNames[];

// Command-buffer debug label helpers (VK_EXT_debug_utils). All of them
// no-op when the extension is absent (e.g. Release builds without
// validation layers): they check the dynamically dispatched function
// pointers before use.
void beginCmdLabel(vk::CommandBuffer commandBuffer, const char *name, const std::array<float, 4> &color);
void endCmdLabel(vk::CommandBuffer commandBuffer);

// RAII wrapper so early returns cannot leave a label region open.
class ScopedCmdLabel
{
  public:
    ScopedCmdLabel(vk::CommandBuffer commandBuffer, const char *name, const std::array<float, 4> &color)
      : cmd(commandBuffer)
    { beginCmdLabel(cmd, name, color); }

    ScopedCmdLabel(const ScopedCmdLabel &) = delete;
    ScopedCmdLabel &operator=(const ScopedCmdLabel &) = delete;

    ~ScopedCmdLabel() { endCmdLabel(cmd); }

  private:
    vk::CommandBuffer cmd;
};

VKAPI_ATTR VkBool32 VKAPI_CALL messageCallback(VkDebugReportFlagsEXT flags,
  VkDebugReportObjectTypeEXT objType,
  uint64_t srcObject,
  size_t location,
  int32_t msgCode,
  const char *pLayerPrefix,
  const char *pMsg,
  void *pUserData);

void setupDebugging(vk::Instance instance, vk::DebugReportFlagsEXT flags, vk::DebugReportCallbackEXT callBack);
void freeDebugCallback(vk::Instance instance);

}// namespace Kataglyphis::debug