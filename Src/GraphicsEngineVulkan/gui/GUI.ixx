module;

#include <memory>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.gui;

import kataglyphis.vulkan.command_buffer_manager;
import kataglyphis.vulkan.device;
import kataglyphis.vulkan.gui_renderer_shared_vars;
import kataglyphis.vulkan.gui_scene_shared_vars;
import kataglyphis.vulkan.window;

export namespace Kataglyphis::Frontend {
class GUI
{
  public:
    GUI(Window *window);

    void initializeVulkanContext(const std::shared_ptr<VulkanDevice> &device,
      const vk::Instance &instance,
      const vk::RenderPass &post_render_pass,
      uint32_t image_count);

    GUISceneSharedVars &getGuiSceneSharedVars() { return guiSceneSharedVars; };
    Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars &getGuiRendererSharedVars()
    { return guiRendererSharedVars; };

    void setUserSelectionForRRT(bool rrtCapabilitiesAvailable);

    void render();

    void cleanUp();

    ~GUI();

  private:
    void create_gui_context(Window *window,
      const vk::Instance &instance,
      const vk::RenderPass &post_render_pass,
      uint32_t image_count);

    std::shared_ptr<VulkanDevice> device{ nullptr };
    Window *window{ nullptr };
    vk::DescriptorPool gui_descriptor_pool{};

    GUISceneSharedVars guiSceneSharedVars;
    Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars guiRendererSharedVars;

    bool renderUserSelectionForRRT = true;
};

}// namespace Kataglyphis::Frontend
