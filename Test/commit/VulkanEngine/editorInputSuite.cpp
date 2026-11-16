// The editor's main flows driven by mouse and key events, and checked by the state they change.

#include "EngineLoadWait.hpp"
#include "ImGuiInputDriver.hpp"
#include "ScopedEnvVar.hpp"
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <vulkan/vulkan.hpp>

#include <memory>
#include <optional>

import kataglyphis.vulkan.camera;
import kataglyphis.vulkan.gui;
import kataglyphis.vulkan.gui_renderer_shared_vars;
import kataglyphis.vulkan.renderer;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.scene_config;
import kataglyphis.vulkan.window;

namespace {

using Kataglyphis::TestSupport::activeChildWindow;
using Kataglyphis::TestSupport::ImGuiInputDriver;
using Kataglyphis::TestSupport::ScopedEnvVar;

bool glfw_reports_vulkan_support()
{
    if (glfwInit() == 0) { return false; }
    const bool supports_vulkan = glfwVulkanSupported() != 0;
    glfwTerminate();
    return supports_vulkan;
}

// Nothing loads, so frames stay cheap and no library row is the loaded one, which a click could not pick.
constexpr const char *MISSING_MODEL = "Models/ShadowTest/no_such_model.obj";
constexpr const char *RIG_MODEL = "Models/ShadowTest/shadow_rig.obj";

// One engine with the editor showing, driven like App::run minus camera input and torn down in the usual order.
struct EditorEngine
{
    std::unique_ptr<Kataglyphis::Frontend::Window> window;
    std::unique_ptr<Kataglyphis::Scene> scene;
    std::unique_ptr<Kataglyphis::Frontend::GUI> gui;
    std::unique_ptr<Camera> camera;
    std::unique_ptr<Kataglyphis::VulkanRenderer> renderer;

    EditorEngine()
      : window(std::make_unique<Kataglyphis::Frontend::Window>(1200, 768)),
        scene(std::make_unique<Kataglyphis::Scene>()), gui(std::make_unique<Kataglyphis::Frontend::GUI>(window.get())),
        camera(std::make_unique<Camera>())
    { renderer = std::make_unique<Kataglyphis::VulkanRenderer>(window.get(), scene.get(), gui.get(), camera.get()); }
    EditorEngine(const EditorEngine &) = delete;
    EditorEngine &operator=(const EditorEngine &) = delete;

    void frame()
    {
        glfwPollEvents();
        gui->render();
        auto &guiSceneSharedVars = gui->getGuiSceneSharedVars();
        renderer->updateStateDueToUserInput(guiSceneSharedVars);
        renderer->updateUniforms(scene.get(), camera.get(), guiSceneSharedVars);
        renderer->drawFrame(guiSceneSharedVars);
    }

    void waitForModel()
    {
        Kataglyphis::TestSupport::waitForModelLoad(renderer.get(), [this] { frame(); });
    }

    // Members go in reverse declaration order, the renderer first, as App::run's locals do.
    ~EditorEngine() = default;
};

// The engine plus a driver feeding its window, past the startup frames that release the menu bar's focus.
struct EditorSession
{
    const ScopedEnvVar missing{ "KATAGLYPHIS_MODEL_OVERRIDE", MISSING_MODEL };
    EditorEngine engine;
    ImGuiInputDriver input{ engine.window->get_window(), [this] { engine.frame(); } };

    EditorSession()
    {
        engine.waitForModel();
        input.frames(2);
    }
};

ImGuiWindow *scene_dock() { return ImGui::FindWindowByName("##studio_scene"); }

// listRow pushes the row's name, then submits its selectable as "##row".
ImGuiID outliner_row_id(const ImGuiWindow &dock, const char *name)
{ return ImGui::GetIDWithSeed("##row", nullptr, ImGui::GetIDWithSeed(name, nullptr, dock.ID)); }

// renderModelLibrary pushes the model's index, listRow then its "##model" string.
ImGuiID library_row_scope(const ImGuiWindow &library, int row)
{ return ImGui::GetIDWithSeed("##model", nullptr, ImGui::GetIDWithSeed(row, library.ID)); }

std::optional<ImVec2> point_at_outliner_row(ImGuiInputDriver &input, const char *name)
{
    const ImGuiWindow *dock = scene_dock();
    if (dock == nullptr) { return std::nullopt; }
    const ImRect &area = dock->InnerClipRect;
    return input.pointAtOnVertical(outliner_row_id(*dock, name), area, area.Min.x + area.GetWidth() * 0.3F);
}

int rig_row() { return sceneConfig::modelIndexOf(sceneConfig::getAvailableModelPaths(), RIG_MODEL); }

bool shows_the_whole_image(const Kataglyphis::VulkanRendererInternals::FrontendShared::SceneViewport &viewport)
{ return viewport.x0 == 0.0F && viewport.y0 == 0.0F && viewport.x1 == 1.0F && viewport.y1 == 1.0F; }

}// namespace

// A load stalls the GPU, so a click only picks a library row; Enter on the clicked row then loads it.
TEST(Integration, LibraryClickPicksARowAndEnterLoadsIt)
{
    if (!glfw_reports_vulkan_support()) { GTEST_SKIP() << "GLFW/Vulkan runtime is unavailable on this system."; }

    EditorSession session;
    auto &engine = session.engine;
    auto &input = session.input;
    ASSERT_TRUE(input.wired()) << "the window installed no GLFW input callbacks";
    ASSERT_EQ(engine.scene->getModelCount(), 0U) << MISSING_MODEL << " loaded after all";
    const int row = rig_row();
    ASSERT_GE(row, 0) << RIG_MODEL << " is not in the model library";

    const ImGuiWindow *library = activeChildWindow("##studio_scene", "##model_library");
    ASSERT_NE(library, nullptr) << "the scene dock drew no model library";
    const ImGuiID row_scope = library_row_scope(*library, row);
    const ImGuiID row_id = ImGui::GetIDWithSeed("##row", nullptr, row_scope);
    const ImRect area = library->InnerClipRect;
    const std::optional<ImVec2> row_point = input.pointAtOnVertical(row_id, area, area.Min.x + area.GetWidth() * 0.3F);
    ASSERT_TRUE(row_point.has_value()) << "no point of the library hovers the shadow rig's row: its ID scope moved";

    input.click(*row_point);
    EXPECT_EQ(ImGui::GetCurrentContext()->LastActiveId, row_id) << "the click reached no library row";
    input.frames(4);
    ASSERT_FALSE(engine.renderer->hasDeviceLost());
    EXPECT_EQ(engine.scene->getModelCount(), 0U) << "a single click loaded the row instead of picking it";

    // A picked row that is not the loaded one offers its Load button at the row's right end.
    const ImGuiID load_id = ImGui::GetIDWithSeed("Load", nullptr, row_scope);
    EXPECT_TRUE(
      input.pointAt(load_id, *row_point, ImVec2(area.Max.x - 1.0F, row_point->y), ImGui::GetFontSize()).has_value())
      << "the clicked row offers no Load button, so the click did not pick it";

    input.pressKey(GLFW_KEY_ENTER);
    input.frames(1);
    ASSERT_FALSE(engine.renderer->hasDeviceLost());
    EXPECT_EQ(engine.scene->getModelCount(), 1U) << "Enter did not load the clicked row";
    EXPECT_TRUE(engine.scene->getLoadedModelPath().ends_with("shadow_rig.obj"))
      << "Enter loaded " << engine.scene->getLoadedModelPath() << ", not the clicked row";
}

// The row's tooltip promises that a double-click loads it.
TEST(Integration, LibraryDoubleClickLoadsTheRow)
{
    if (!glfw_reports_vulkan_support()) { GTEST_SKIP() << "GLFW/Vulkan runtime is unavailable on this system."; }

    EditorSession session;
    auto &engine = session.engine;
    auto &input = session.input;
    ASSERT_TRUE(input.wired()) << "the window installed no GLFW input callbacks";
    ASSERT_EQ(engine.scene->getModelCount(), 0U) << MISSING_MODEL << " loaded after all";
    const int row = rig_row();
    ASSERT_GE(row, 0) << RIG_MODEL << " is not in the model library";

    const ImGuiWindow *library = activeChildWindow("##studio_scene", "##model_library");
    ASSERT_NE(library, nullptr) << "the scene dock drew no model library";
    const ImGuiID row_id = ImGui::GetIDWithSeed("##row", nullptr, library_row_scope(*library, row));
    const ImRect area = library->InnerClipRect;
    const std::optional<ImVec2> row_point = input.pointAtOnVertical(row_id, area, area.Min.x + area.GetWidth() * 0.3F);
    ASSERT_TRUE(row_point.has_value()) << "no point of the library hovers the shadow rig's row: its ID scope moved";

    input.doubleClick(*row_point);
    input.frames(1);
    ASSERT_FALSE(engine.renderer->hasDeviceLost());
    EXPECT_EQ(engine.scene->getModelCount(), 1U) << "a double-click on the row did not load it";
    EXPECT_TRUE(engine.scene->getLoadedModelPath().ends_with("shadow_rig.obj"))
      << "the double-click loaded " << engine.scene->getLoadedModelPath();
}

// Clicking an outliner row opens its inspector tab; the Renderer tab's culling switch then reaches the renderer.
TEST(Integration, OutlinerRendererRowOpensTheTabWhoseSwitchTogglesCulling)
{
    if (!glfw_reports_vulkan_support()) { GTEST_SKIP() << "GLFW/Vulkan runtime is unavailable on this system."; }

    EditorSession session;
    auto &engine = session.engine;
    auto &input = session.input;
    ASSERT_TRUE(input.wired()) << "the window installed no GLFW input callbacks";
    auto &renderer_vars = engine.gui->getGuiRendererSharedVars();
    ASSERT_TRUE(renderer_vars.frustum_culling_enabled) << "culling must be on at startup";

    const std::optional<ImVec2> renderer_row = point_at_outliner_row(input, "Renderer");
    ASSERT_TRUE(renderer_row.has_value()) << "no point of the scene dock hovers the outliner's Renderer row";
    input.click(*renderer_row);
    // The forced tab switch shows the old tab for one frame.
    input.frames(2);

    const ImGuiWindow *body = activeChildWindow("##studio_inspector", "##inspector_body");
    ASSERT_NE(body, nullptr) << "the inspector drew no tab body";
    // captionLine right-aligns the switch, so it ends where the body's work rect does.
    const ImGuiID culling_switch = ImGui::GetIDWithSeed("##frustum_culling", nullptr, body->ID);
    const std::optional<ImVec2> switch_point =
      input.pointAtOnVertical(culling_switch, body->InnerClipRect, body->WorkRect.Max.x - 4.0F);
    ASSERT_TRUE(switch_point.has_value())
      << "the inspector's shown tab has no culling switch: the Renderer row did not open the Renderer tab";

    input.click(*switch_point);
    input.frames(1);
    EXPECT_FALSE(renderer_vars.frustum_culling_enabled) << "the culling switch did not turn culling off";
    input.click(*switch_point);
    input.frames(1);
    EXPECT_TRUE(renderer_vars.frustum_culling_enabled) << "a second press did not turn culling back on";
    ASSERT_FALSE(engine.renderer->hasDeviceLost());
}

// View > Inspector closes the dock, which hands the scene its width; any outliner row brings the inspector back.
TEST(Integration, ViewMenuClosesTheInspectorAndAnOutlinerRowReopensIt)
{
    if (!glfw_reports_vulkan_support()) { GTEST_SKIP() << "GLFW/Vulkan runtime is unavailable on this system."; }

    EditorSession session;
    auto &engine = session.engine;
    auto &input = session.input;
    ASSERT_TRUE(input.wired()) << "the window installed no GLFW input callbacks";
    const auto &viewport = engine.gui->getGuiRendererSharedVars().scene_viewport;
    ASSERT_LT(viewport.x1, 1.0F) << "the inspector dock must cover the image's right edge at startup";

    ImGuiWindow *menu_bar = ImGui::FindWindowByName("##MainMenuBar");
    ASSERT_NE(menu_bar, nullptr) << "the editor drew no main menu bar";
    const ImGuiID view_menu =
      ImGui::GetIDWithSeed("View", nullptr, ImGui::GetIDWithSeed("##MenuBar", nullptr, menu_bar->ID));
    const float bar_y = menu_bar->Pos.y + menu_bar->Size.y * 0.5F;
    const std::optional<ImVec2> view_point = input.pointAt(view_menu,
      ImVec2(menu_bar->Pos.x + 1.0F, bar_y),
      ImVec2(menu_bar->Pos.x + menu_bar->Size.x - 1.0F, bar_y),
      ImGui::GetFontSize() * 0.75F);
    ASSERT_TRUE(view_point.has_value()) << "no point of the menu bar hovers its View menu";
    input.click(*view_point);

    // BeginMenu recycles one popup window per depth.
    const ImGuiWindow *view_popup = ImGui::FindWindowByName("###Menu_00");
    ASSERT_TRUE(view_popup != nullptr && view_popup->Active) << "clicking View opened no menu";
    const ImGuiID inspector_item = ImGui::GetIDWithSeed("Inspector", nullptr, view_popup->ID);
    const ImRect popup_area = view_popup->InnerClipRect;
    const std::optional<ImVec2> item_point =
      input.pointAtOnVertical(inspector_item, popup_area, popup_area.Min.x + popup_area.GetWidth() * 0.3F);
    ASSERT_TRUE(item_point.has_value()) << "the View menu shows no Inspector item";
    input.click(*item_point);
    input.frames(1);
    EXPECT_FLOAT_EQ(viewport.x1, 1.0F) << "View > Inspector left the inspector dock open";

    const std::optional<ImVec2> sun_row = point_at_outliner_row(input, "Sun");
    ASSERT_TRUE(sun_row.has_value()) << "no point of the scene dock hovers the outliner's Sun row";
    input.click(*sun_row);
    input.frames(1);
    EXPECT_LT(viewport.x1, 1.0F) << "selecting an outliner row did not bring the inspector back";
    ASSERT_FALSE(engine.renderer->hasDeviceLost());
}

// The eye is a button inside the Skybox row, so the row's own selectable must not take its press.
TEST(Integration, OutlinerEyeTogglesTheSkybox)
{
    if (!glfw_reports_vulkan_support()) { GTEST_SKIP() << "GLFW/Vulkan runtime is unavailable on this system."; }

    EditorSession session;
    auto &engine = session.engine;
    auto &input = session.input;
    ASSERT_TRUE(input.wired()) << "the window installed no GLFW input callbacks";
    auto &scene_vars = engine.gui->getGuiSceneSharedVars();
    ASSERT_TRUE(scene_vars.skybox_enabled) << "the skybox must show at startup";

    const ImGuiWindow *dock = scene_dock();
    ASSERT_NE(dock, nullptr) << "the editor drew no scene dock";
    const ImGuiID eye = ImGui::GetIDWithSeed("##visible", nullptr, ImGui::GetIDWithSeed("Skybox", nullptr, dock->ID));
    // listRow centres the eye's glyph a frame padding and half a glyph inside the row's right end.
    const float eye_x = dock->WorkRect.Max.x - ImGui::GetStyle().FramePadding.x - ImGui::GetFontSize() * 0.5F;
    const std::optional<ImVec2> eye_point = input.pointAtOnVertical(eye, dock->InnerClipRect, eye_x);
    ASSERT_TRUE(eye_point.has_value()) << "no point of the scene dock hovers the Skybox row's eye";

    input.click(*eye_point);
    input.frames(1);
    EXPECT_FALSE(scene_vars.skybox_enabled) << "the eye did not hide the skybox";
    input.click(*eye_point);
    input.frames(1);
    EXPECT_TRUE(scene_vars.skybox_enabled) << "a second press did not show the skybox again";
    ASSERT_FALSE(engine.renderer->hasDeviceLost());
}

// F1 hides the editor, which gives the scene the whole image, and brings it back, as the status bar says.
TEST(Integration, F1HidesAndRestoresTheEditor)
{
    if (!glfw_reports_vulkan_support()) { GTEST_SKIP() << "GLFW/Vulkan runtime is unavailable on this system."; }

    EditorSession session;
    auto &engine = session.engine;
    auto &input = session.input;
    ASSERT_TRUE(input.wired()) << "the window installed no GLFW input callbacks";
    const auto &viewport = engine.gui->getGuiRendererSharedVars().scene_viewport;
    ASSERT_FALSE(shows_the_whole_image(viewport)) << "the editor's bars must inset the scene at startup";

    input.pressKey(GLFW_KEY_F1);
    input.frames(1);
    EXPECT_FALSE(engine.gui->isOverlayVisible()) << "F1 did not hide the editor";
    EXPECT_TRUE(shows_the_whole_image(viewport)) << "the hidden editor still insets the scene";

    input.pressKey(GLFW_KEY_F1);
    input.frames(1);
    EXPECT_TRUE(engine.gui->isOverlayVisible()) << "a second F1 did not bring the editor back";
    EXPECT_FALSE(shows_the_whole_image(viewport)) << "the restored editor does not inset the scene";
    ASSERT_FALSE(engine.renderer->hasDeviceLost());
}
