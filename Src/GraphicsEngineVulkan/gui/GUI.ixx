module;

#include <memory>
#include <string_view>
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
      vk::Format post_color_format,
      uint32_t image_count);

    GUISceneSharedVars &getGuiSceneSharedVars() { return guiSceneSharedVars; };
    Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars &getGuiRendererSharedVars()
    { return guiRendererSharedVars; };

    void setUserSelectionForRRT(bool rrtCapabilitiesAvailable);

    // Hidden, the GUI still runs its frame but draws nothing and gives the scene the whole image (the goldens).
    void setOverlayVisible(bool visible) { overlayVisible = visible; }
    [[nodiscard]] bool isOverlayVisible() const { return overlayVisible; }

    void render();

    // What the renderer draws, per drawnRenderMode(): a ray- or path-tracing pick shows raster until a TLAS exists.
    [[nodiscard]] int displayedRenderMode() const;
    // The scene's model as the editor names it, listed or not; empty while the scene holds none.
    [[nodiscard]] std::string_view loadedModelName() const;

    void cleanUp();

    ~GUI();

  private:
    struct StudioState;

    void create_gui_context(Window *window,
      const vk::Instance &instance,
      vk::Format post_color_format,
      uint32_t image_count);

    [[nodiscard]] int renderMode() const;
    void setRenderMode(int mode);
    [[nodiscard]] bool tracingAvailable() const;
    // Both render-mode controls: one rule for what they show and what a press picks.
    void renderModeSwitch(const char *id, float width);

    void renderDocks(float uiScale);
    void renderMenuBar();
    void renderStatusBar();
    void renderSceneDock(float width);
    void renderModelLibrary();
    void renderInspectorDock(float width);
    void renderProfilerDock(float height);
    void renderLightInspector();
    void renderCameraInspector();
    void renderModelInspector();
    void renderSkyInspector();
    void renderRenderInspector();
    void renderProfilerTab();
    void renderVisibilityTab();
    void renderViewportOverlay(float sceneX0, float sceneY0, float sceneX1, float sceneY1);
    void renderFloatingWindows();

    std::shared_ptr<VulkanDevice> device{ nullptr };
    Window *window{ nullptr };
    vk::DescriptorPool gui_descriptor_pool{};

    GUISceneSharedVars guiSceneSharedVars;
    Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars guiRendererSharedVars;

    bool renderUserSelectionForRRT = true;
    bool overlayVisible = true;
    std::unique_ptr<StudioState> studio;
};

}// namespace Kataglyphis::Frontend
