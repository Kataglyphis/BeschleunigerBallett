module;

#include <array>
#include <cstdint>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.debug;

export namespace Kataglyphis::debug {
extern int validationLayerCount;
extern const char *validationLayerNames[];

// Sees every label scope this thread records, with or without debug_utils, so the device-loss breadcrumbs follow them.
class CmdLabelObserver
{
  public:
    CmdLabelObserver() = default;
    CmdLabelObserver(const CmdLabelObserver &) = delete;
    CmdLabelObserver &operator=(const CmdLabelObserver &) = delete;
    CmdLabelObserver(CmdLabelObserver &&) = delete;
    CmdLabelObserver &operator=(CmdLabelObserver &&) = delete;
    virtual ~CmdLabelObserver() = default;

    virtual void labelBegun(vk::CommandBuffer commandBuffer, const char *name) = 0;
    virtual void labelEnded(vk::CommandBuffer commandBuffer) = 0;
};

// Installs observer for this thread until it goes, then restores the one before.
class ScopedCmdLabelObserver
{
  public:
    explicit ScopedCmdLabelObserver(CmdLabelObserver *observer);
    ScopedCmdLabelObserver(const ScopedCmdLabelObserver &) = delete;
    ScopedCmdLabelObserver &operator=(const ScopedCmdLabelObserver &) = delete;
    ScopedCmdLabelObserver(ScopedCmdLabelObserver &&) = delete;
    ScopedCmdLabelObserver &operator=(ScopedCmdLabelObserver &&) = delete;
    ~ScopedCmdLabelObserver();

  private:
    CmdLabelObserver *previous;
};

// Command-buffer debug label helpers (VK_EXT_debug_utils). They record no
// label when the instance did not enable the extension (its loader lists
// none), but still tell this thread's CmdLabelObserver.
void beginCmdLabel(vk::CommandBuffer commandBuffer, const char *name, const std::array<float, 4> &color);
void endCmdLabel(vk::CommandBuffer commandBuffer);

// True once an instance enabled debug_utils, with or without the validation layer.
bool labelsAvailable();

// Names an object for captures and validation messages; false when debug_utils is off or the driver refused.
bool setObjectName(vk::Device device, vk::ObjectType type, std::uint64_t handle, const char *name);

// Any vulkan.hpp handle type; a null handle has nothing to name.
template<typename Handle> bool nameObject(vk::Device device, Handle handle, const char *name)
{
    if (!handle) { return false; }
    return setObjectName(
      device, Handle::objectType, reinterpret_cast<std::uint64_t>(static_cast<typename Handle::CType>(handle)), name);
}

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