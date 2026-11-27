// The editor's main flows driven by mouse and key events, and checked by the state they change.

#include "EngineFrame.hpp"
#include "EngineLoadWait.hpp"
#include "GpuTestGuard.hpp"
#include "ImGuiInputDriver.hpp"
#include "ScopedEnvVar.hpp"
#include "ScopedTempDir.hpp"
#include "common/Globals.hpp"
#include "common/SceneUboMarshal.hpp"
#include "gui/DoubleClickWindow.hpp"
#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <stb_image.h>
#include <vulkan/vulkan.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

import kataglyphis.shared.util.utf8_path;
import kataglyphis.vulkan.camera;
import kataglyphis.vulkan.gui;
import kataglyphis.vulkan.render_settings;
import kataglyphis.vulkan.renderer;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.scene_config;
import kataglyphis.vulkan.window;

namespace {

using Kataglyphis::TestSupport::activeChildWindow;
using Kataglyphis::TestSupport::ImGuiInputDriver;
using Kataglyphis::TestSupport::ScopedEnvVar;
using Kataglyphis::TestSupport::ScopedTempDir;

// Nothing loads, so frames stay cheap and no library row is the loaded one, which a click could not pick.
constexpr const char *MISSING_MODEL = "Models/ShadowTest/no_such_model.obj";
constexpr const char *RIG_MODEL = "Models/ShadowTest/shadow_rig.obj";
constexpr const char *CARD_MODEL = "Models/GltfTest/two_primitives.gltf";

// One engine with the editor showing, driven like App::run minus camera input and torn down in the usual order.
struct EditorEngine
{
    Kataglyphis::RenderSettings settings;
    Kataglyphis::RendererFeedback feedback;
    std::unique_ptr<Kataglyphis::Frontend::Window> window;
    std::unique_ptr<Kataglyphis::Scene> scene;
    std::unique_ptr<Kataglyphis::Frontend::GUI> gui;
    std::unique_ptr<Camera> camera;
    std::unique_ptr<Kataglyphis::VulkanRenderer> renderer;

    EditorEngine()
      : window(std::make_unique<Kataglyphis::Frontend::Window>(1200, 768)),
        scene(std::make_unique<Kataglyphis::Scene>()),
        gui(std::make_unique<Kataglyphis::Frontend::GUI>(window.get(), settings, feedback)),
        camera(std::make_unique<Camera>())
    {
        renderer =
          std::make_unique<Kataglyphis::VulkanRenderer>(window.get(), scene.get(), camera.get(), settings, feedback);
        Kataglyphis::TestSupport::attachEditor(*gui, *renderer);
    }
    EditorEngine(const EditorEngine &) = delete;
    EditorEngine &operator=(const EditorEngine &) = delete;

    void frame()
    {
        glfwPollEvents();
        Kataglyphis::TestSupport::renderEditorFrame(*gui, *renderer, scene.get(), camera.get(), settings, feedback);
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
    explicit EditorSession(const char *model = MISSING_MODEL) : startup{ "KATAGLYPHIS_MODEL_OVERRIDE", model }
    {
        engine.waitForModel();
        input.frames(2);
    }

    const ScopedEnvVar startup;
    EditorEngine engine;
    ImGuiInputDriver input{ engine.window->get_window(), [this] { engine.frame(); } };
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
    SKIP_WITHOUT_GPU();

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
    SKIP_WITHOUT_GPU();

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

// The window must outlast the two frames between the presses at any frame rate: riscv64's QEMU frames passed a 2 s cap.
TEST(DoubleClickWindowUnit, OutlastsTheTwoFramesBetweenThePressesAtAnyFrameRate)
{
    using Kataglyphis::Frontend::doubleClickWindow;
    EXPECT_FLOAT_EQ(doubleClickWindow(1.0F / 60.0F, 1.0F / 60.0F), 0.3F) << "at 60 FPS it is ImGui's default";
    for (const float frame : { 0.05F, 0.4F, 1.0F, 2.5F, 8.0F }) {
        EXPECT_GT(doubleClickWindow(frame, frame), 2.0F * frame) << "two frames of " << frame << " s";
    }
}

// Clicking an outliner row opens its inspector tab; the Renderer tab's culling switch then reaches the renderer.
TEST(Integration, OutlinerRendererRowOpensTheTabWhoseSwitchTogglesCulling)
{
    SKIP_WITHOUT_GPU();

    EditorSession session;
    auto &engine = session.engine;
    auto &input = session.input;
    ASSERT_TRUE(input.wired()) << "the window installed no GLFW input callbacks";
    auto &renderer_vars = engine.settings.renderer;
    ASSERT_TRUE(renderer_vars.frustum_culling_enabled) << "culling must be on at startup";

    const std::optional<ImVec2> renderer_row = point_at_outliner_row(input, "Renderer");
    ASSERT_TRUE(renderer_row.has_value()) << "no point of the scene dock hovers the outliner's Renderer row";
    input.click(*renderer_row);
    // The forced tab switch shows the old tab for one frame.
    input.frames(2);

    const ImGuiWindow *body = activeChildWindow("##studio_inspector", "##inspector_body");
    ASSERT_NE(body, nullptr) << "the inspector drew no tab body";
    // The tab outgrows the dock at 1200 x 768, and only SHADERS follows the switch, so the body's end shows it.
    input.wheel(body->InnerClipRect.GetCenter(), -100.0);
    // captionLine right-aligns the switch, so it ends where the body's work rect does.
    const ImGuiID culling_switch = ImGui::GetIDWithSeed("##frustum_culling", nullptr, body->ID);
    const std::optional<ImVec2> switch_point =
      input.pointAtOnVertical(culling_switch, body->InnerClipRect, body->WorkRect.Max.x - 4.0F);
    ASSERT_TRUE(switch_point.has_value())
      << "the inspector's shown tab has no culling switch: the Renderer row did not open the Renderer tab, or the "
         "wheel left it out of view (scrolled "
      << body->Scroll.y << " of " << body->ScrollMax.y << " px)";

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
    SKIP_WITHOUT_GPU();

    EditorSession session;
    auto &engine = session.engine;
    auto &input = session.input;
    ASSERT_TRUE(input.wired()) << "the window installed no GLFW input callbacks";
    const auto &viewport = engine.settings.renderer.scene_viewport;
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
    SKIP_WITHOUT_GPU();

    EditorSession session;
    auto &engine = session.engine;
    auto &input = session.input;
    ASSERT_TRUE(input.wired()) << "the window installed no GLFW input callbacks";
    auto &scene_vars = engine.settings.scene;
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
    SKIP_WITHOUT_GPU();

    EditorSession session;
    auto &engine = session.engine;
    auto &input = session.input;
    ASSERT_TRUE(input.wired()) << "the window installed no GLFW input callbacks";
    const auto &viewport = engine.settings.renderer.scene_viewport;
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

// F5 asks for a shader reload in every build, not only Debug (owner 2026-11-19); the Release commit suites run this.
TEST(Integration, F5RequestsAShaderReload)
{
    SKIP_WITHOUT_GPU();

    EditorSession session;
    auto &engine = session.engine;
    // The renderer spends the request in the frame that raised it, so it is read between the editor and the renderer.
    int requests = 0;
    const auto frame = [&engine, &requests] {
        glfwPollEvents();
        engine.gui->render();
        if (engine.settings.renderer.shader_hot_reload_triggered) { ++requests; }
        engine.renderer->updateStateDueToUserInput(engine.settings, engine.feedback);
        Kataglyphis::applyRendererFeedback(engine.settings, engine.feedback);
        engine.renderer->updateUniforms(engine.scene.get(), engine.camera.get(), engine.settings);
        engine.renderer->drawFrame(engine.settings, engine.feedback);
    };
    ImGuiInputDriver input{ engine.window->get_window(), frame };
    ASSERT_TRUE(input.wired()) << "the window installed no GLFW input callbacks";

    input.frames(2);
    ASSERT_EQ(requests, 0) << "a shader reload was requested before any key";
    input.pressKey(GLFW_KEY_F5);
    EXPECT_EQ(requests, 1) << "F5 did not ask the renderer to reload the shaders";
    EXPECT_FALSE(engine.settings.renderer.shader_hot_reload_triggered) << "the renderer did not spend the request";
    ASSERT_FALSE(engine.renderer->hasDeviceLost());
}

// F12 with the editor up saves the scene viewport, the rect the projection centres on, before the editor draws on it.
TEST(Integration, F12SavesTheSceneViewportWithoutTheEditor)
{
    SKIP_WITHOUT_GPU();

    EditorSession session(RIG_MODEL);
    auto &engine = session.engine;
    auto &input = session.input;
    SKIP_WITHOUT_FRAME_CAPTURE(*engine.renderer);
    ASSERT_TRUE(input.wired()) << "the window installed no GLFW input callbacks";
    ASSERT_EQ(engine.scene->getModelCount(), 1U) << "the rig did not load";
    const ScopedTempDir dir("f12-editor");
    ASSERT_FALSE(dir.path().empty());
    engine.renderer->setScreenshotDirectory(dir.path());

    input.pressKey(GLFW_KEY_F12);
    for (int frame = 0; frame < Kataglyphis::MAX_FRAME_DRAWS + 1 && engine.renderer->isScreenshotPending(); ++frame) {
        engine.frame();
    }
    EXPECT_FALSE(engine.renderer->isScreenshotPending()) << "the copy was not collected within the frames in flight";
    engine.renderer->waitForScreenshots();

    // The same still scene with the editor over it.
    uint32_t width = 0;
    uint32_t height = 0;
    engine.renderer->requestFrameCapture();
    engine.frame();
    const std::vector<uint8_t> frame = engine.renderer->takeCapturedFrame(width, height);
    ASSERT_EQ(frame.size(), static_cast<size_t>(width) * height * 4U);
    ASSERT_FALSE(engine.renderer->hasDeviceLost());

    const auto &viewport = engine.settings.renderer.scene_viewport;
    const Kataglyphis::ViewportPixels rect = Kataglyphis::viewportPixels(
      width, height, Kataglyphis::sanitizeViewportFraction(viewport.x0, viewport.y0, viewport.x1, viewport.y1));
    ASSERT_LT(rect.width, width) << "the docks leave the scene the whole image, so the shot proves nothing";

    int png_width = 0;
    int png_height = 0;
    int channels = 0;
    stbi_uc *png = stbi_load(
      (dir.path() / "screenshot-000.png").string().c_str(), &png_width, &png_height, &channels, STBI_rgb_alpha);
    ASSERT_NE(png, nullptr) << "F12 wrote no screenshot-000.png";
    EXPECT_EQ(png_width, static_cast<int>(rect.width));
    EXPECT_EQ(png_height, static_cast<int>(rect.height));
    if (png_width == static_cast<int>(rect.width) && png_height == static_cast<int>(rect.height)) {
        // The mode chip sits in the rect's top band; the splitters may draw along its edges.
        const ImGuiIO &io = ImGui::GetIO();
        const auto band = static_cast<uint32_t>(std::ceil((ImGui::GetStyle().WindowPadding.y + ImGui::GetFrameHeight())
                                                          * io.DisplayFramebufferScale.y))
                          + 2U;
        const uint32_t margin = 12U;
        size_t chip_pixels = 0;
        size_t scene_pixels = 0;
        size_t scene_off = 0;
        for (uint32_t y = 0; y < rect.height; ++y) {
            for (uint32_t x = margin; x + margin < rect.width; ++x) {
                const size_t shot = (static_cast<size_t>(y) * rect.width + x) * 4U;
                const size_t full = ((static_cast<size_t>(rect.y) + y) * width + rect.x + x) * 4U;
                bool off = false;
                for (size_t channel = 0; channel < 3U; ++channel) {
                    off =
                      off
                      || std::abs(static_cast<int>(png[shot + channel]) - static_cast<int>(frame[full + channel])) > 2;
                }
                if (y < band) {
                    chip_pixels += off ? 1U : 0U;
                } else if (y + margin < rect.height) {
                    ++scene_pixels;
                    scene_off += off ? 1U : 0U;
                }
            }
        }
        EXPECT_LE(scene_off, scene_pixels / 1000U) << scene_off << " of " << scene_pixels << " scene pixels differ";
        EXPECT_GE(chip_pixels, 100U) << "the shot shows the editor's mode chip, so it was copied after the editor drew";
    }
    stbi_image_free(png);
}

// A dropped model replaces the scene through the asynchronous load, while the old one draws; a bad drop keeps it.
TEST(Integration, ADroppedModelReplacesTheSceneThroughTheAsynchronousLoad)
{
    SKIP_WITHOUT_GPU();

    EditorSession session(RIG_MODEL);
    auto &engine = session.engine;
    auto &input = session.input;
    ASSERT_TRUE(input.wired()) << "the window installed no GLFW input callbacks";
    ASSERT_EQ(engine.scene->getLoadedModelPath(), RIG_MODEL) << "the rig did not load";
    const std::filesystem::path card = sceneConfig::resolveModelPath(CARD_MODEL);
    ASSERT_TRUE(std::filesystem::exists(card)) << card.string();
    const int card_row = sceneConfig::modelIndexOf(sceneConfig::getAvailableModelPaths(), CARD_MODEL);
    ASSERT_GE(card_row, 0) << CARD_MODEL << " is not in the model library";

    // The last model of a drop wins; a file that is no model is passed over.
    const std::string card_path = card.string();
    const std::array<const char *, 2> dropped{ "notes.txt", card_path.c_str() };
    input.drop(dropped);
    EXPECT_TRUE(engine.renderer->isModelLoadPending()) << "the drop started no asynchronous load";
    EXPECT_EQ(engine.scene->getLoadedModelPath(), RIG_MODEL) << "the rig must draw until the card is in";
    engine.waitForModel();
    ASSERT_FALSE(engine.renderer->hasDeviceLost());
    EXPECT_EQ(engine.scene->getModelCount(), 1U) << "the drop added the card instead of replacing the rig";
    EXPECT_EQ(
      sceneConfig::modelIndexOf(std::array<std::string, 1>{ CARD_MODEL }, engine.scene->getLoadedModelPath()), 0)
      << "loaded " << engine.scene->getLoadedModelPath() << ", not the library's path of the card";
    input.frames(1);
    EXPECT_EQ(engine.settings.scene.selected_model_index, card_row) << "the library does not mark the dropped card";

    const ScopedTempDir dir("drop-missing");
    ASSERT_FALSE(dir.path().empty());
    const std::string missing = (dir.path() / "gone.gltf").string();
    const std::array<const char *, 1> gone{ missing.c_str() };
    input.drop(gone);
    engine.waitForModel();
    EXPECT_EQ(engine.scene->getModelCount(), 1U) << "a drop that does not load emptied the scene";
    EXPECT_EQ(
      sceneConfig::modelIndexOf(std::array<std::string, 1>{ CARD_MODEL }, engine.scene->getLoadedModelPath()), 0);
    ASSERT_FALSE(engine.renderer->hasDeviceLost());

    // No ANSI code page holds the emoji: a Windows narrow path aborted on it; a UTF-8 file system loads it.
    const std::filesystem::path odd = dir.path() / std::filesystem::path(u8"card-\U0001F600.gltf");
    std::error_code error;
    ASSERT_TRUE(std::filesystem::copy_file(card, odd, error)) << error.message();
    const std::optional<std::string> narrow = Kataglyphis::Shared::narrowPath(odd);
    const std::string odd_utf8 = Kataglyphis::Shared::utf8PathText(odd);
    const std::array<const char *, 1> odd_drop{ odd_utf8.c_str() };
    input.drop(odd_drop);
    engine.waitForModel();
    ASSERT_FALSE(engine.renderer->hasDeviceLost());
    EXPECT_EQ(engine.scene->getModelCount(), 1U);
    if (narrow.has_value()) {
        EXPECT_TRUE(std::filesystem::equivalent(std::filesystem::path(engine.scene->getLoadedModelPath()), odd, error))
          << "loaded " << engine.scene->getLoadedModelPath() << ", not the emoji-named card";
    } else {
        EXPECT_EQ(
          sceneConfig::modelIndexOf(std::array<std::string, 1>{ CARD_MODEL }, engine.scene->getLoadedModelPath()), 0)
          << "a name the code page cannot hold replaced the scene";
    }
}

// The Camera tab's Fly speed is how far W travels a second: a press on its fill sets it, the renderer the camera's.
TEST(Integration, TheFlySpeedSetsTheCamerasTravelPerSecond)
{
    SKIP_WITHOUT_GPU();

    EditorSession session;
    auto &engine = session.engine;
    auto &input = session.input;
    ASSERT_TRUE(input.wired()) << "the window installed no GLFW input callbacks";
    const std::optional<ImVec2> camera_row = point_at_outliner_row(input, "Camera");
    ASSERT_TRUE(camera_row.has_value()) << "no point of the scene dock hovers the outliner's Camera row";
    input.click(*camera_row);
    input.frames(2);
    const ImGuiWindow *body = activeChildWindow("##studio_inspector", "##inspector_body");
    ASSERT_NE(body, nullptr) << "the inspector drew no tab body";
    // beginStudioProperties' table pushes its id; the slider fills the value column.
    const ImGuiID lens = ImGui::GetIDWithSeed("##lens", nullptr, body->ID);
    const ImGuiTable *table = ImGui::TableFindByID(lens);
    ASSERT_NE(table, nullptr) << "the Camera row did not open the Camera tab";
    const float left = table->Columns[1].WorkMinX;
    const float right = table->Columns[1].WorkMaxX;
    const std::optional<ImVec2> on_slider =
      input.pointAtOnVertical(ImGui::GetIDWithSeed("##fly_speed", nullptr, lens), body->InnerClipRect, right - 4.0F);
    ASSERT_TRUE(on_slider.has_value()) << "the Camera tab has no Fly speed slider";

    std::array<bool, GLFW_KEY_LAST + 1> keys{};
    keys[GLFW_KEY_W] = true;
    for (const float wanted : { 40.0F, 0.25F }) {
        const float fill = std::log(wanted / kMinCameraFlySpeed) / std::log(kMaxCameraFlySpeed / kMinCameraFlySpeed);
        input.click(ImVec2(left + fill * (right - left), on_slider->y));
        engine.frame();
        const float speed = engine.settings.scene.camera_fly_speed;
        EXPECT_NEAR(std::log(speed / wanted), 0.0F, std::log(1.25F))
          << "a press where the fill of " << wanted << " ends set " << speed;
        EXPECT_FLOAT_EQ(engine.camera->get_movement_speed(), speed);
        const glm::vec3 start = engine.camera->get_camera_position();
        engine.camera->key_control(keys, 0.5F);
        EXPECT_NEAR(glm::length(engine.camera->get_camera_position() - start), speed * 0.5F, speed * 1e-4F);
    }
    // A value no slider sets, as from a config, flies at the default.
    engine.settings.scene.camera_fly_speed = std::nanf("");
    engine.frame();
    EXPECT_FLOAT_EQ(engine.camera->get_movement_speed(), kDefaultCameraFlySpeed);
    ASSERT_FALSE(engine.renderer->hasDeviceLost());
}
