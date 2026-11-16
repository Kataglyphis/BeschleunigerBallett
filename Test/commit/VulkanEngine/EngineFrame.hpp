#pragma once

namespace Kataglyphis::TestSupport {

/// App::run's wiring once the renderer exists: the editor's ImGui backend runs on the renderer's device.
/// Templates keep this header import-free, as EngineLoadWait.hpp explains.
template<typename Gui, typename Renderer> void attachEditor(Gui &gui, const Renderer &renderer)
{
    gui.initializeVulkanContext(
      renderer.sharedDevice(), renderer.vulkanInstance(), renderer.swapChainFormat(), renderer.swapChainImageCount());
    gui.setUserSelectionForRRT(renderer.supportsHardwareRaytracing());
}

/// One iteration of App::run's frame loop after input; applyRendererFeedback is found by argument-dependent lookup.
template<typename Gui, typename Renderer, typename Scene, typename Camera, typename Settings, typename Feedback>
void renderEditorFrame(Gui &gui,
  Renderer &renderer,
  Scene *scene,
  Camera *camera,
  Settings &settings,
  Feedback &feedback)
{
    gui.render();
    renderer.updateStateDueToUserInput(settings, feedback);
    applyRendererFeedback(settings, feedback);
    renderer.updateUniforms(scene, camera, settings);
    renderer.drawFrame(settings, feedback);
}

}// namespace Kataglyphis::TestSupport
