module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/SceneUboMarshal.hpp"
#include "common/Utilities.hpp"
#include "common/host_device_shared_vars.hpp"
#include "scene/atmospheric_effects/clouds/CloudDispatch.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <imgui_internal.h>
#include <vulkan/vulkan.hpp>

module kataglyphis.vulkan.gui;

import kataglyphis.shared.frontend.common_gui_panels;
import kataglyphis.shared.imgui.fonts;
import kataglyphis.shared.imgui.layout;
import kataglyphis.shared.imgui.style;
import kataglyphis.shared.imgui.widgets;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.queue_family_indices;
import kataglyphis.vulkan.window;
import kataglyphis.vulkan.scene_config;

using namespace Kataglyphis::Frontend;

// Named rather than anonymous: GUI::StudioState below is a member of an exported class and holds these.
namespace Kataglyphis::Frontend::gui_detail {
enum class InspectorTab : std::uint8_t { Model, Sun, Camera, Sky, Renderer };
enum class SceneObject : std::uint8_t { Model, Sun, Camera, Skybox, Clouds, Renderer };

// Dock extents at UI scale 1. The splitters move them; the window caps them, so the scene keeps the most.
inline constexpr float kDefaultSceneDockWidth = 248.0F;
inline constexpr float kDefaultInspectorDockWidth = 336.0F;
inline constexpr float kDefaultProfilerDockHeight = 172.0F;
inline constexpr float kMinSceneDockWidth = 180.0F;
inline constexpr float kMinInspectorDockWidth = 260.0F;
inline constexpr float kMinProfilerDockHeight = 116.0F;
inline constexpr float kMaxSceneDockShare = 0.3F;
inline constexpr float kMaxInspectorDockShare = 0.4F;
inline constexpr float kMaxProfilerDockShare = 0.45F;
inline constexpr float kSplitterThickness = 6.0F;
// The least of the scene a dock may leave; a smaller window drops docks instead of overlapping them.
inline constexpr float kMinSceneViewWidth = 320.0F;
inline constexpr float kMinSceneViewHeight = 180.0F;

inline constexpr std::size_t kFrameTimeHistory = 120;
}// namespace Kataglyphis::Frontend::gui_detail

namespace Kataglyphis::Frontend {
struct GUI::StudioState
{
    KataglyphisFonts fonts{};
    // 0 makes the first render() apply the theme at the window's real content scale.
    float appliedScale = 0.0F;
    float userScale = 1.0F;
    float previousDeltaTime = 0.0F;

    bool showScene = true;
    bool showInspector = true;
    bool showProfiler = true;
    bool showStyleEditor = false;
    bool showMetrics = false;
    bool openAbout = false;
    bool releaseFocus = true;

    float sceneWidth = gui_detail::kDefaultSceneDockWidth;
    float inspectorWidth = gui_detail::kDefaultInspectorDockWidth;
    float profilerHeight = gui_detail::kDefaultProfilerDockHeight;
    StudioSplitterDrag sceneDrag;
    StudioSplitterDrag inspectorDrag;
    StudioSplitterDrag profilerDrag;

    gui_detail::SceneObject selection = gui_detail::SceneObject::Model;
    // Set when the selection changed outside the tab bar, and at start (a new bar opens tab one), so the bar follows.
    bool selectionChanged = true;

    // Indices into sceneConfig's model list in display order; the list itself stays the renderer's order.
    std::vector<std::size_t> libraryOrder;
    // The library row a click picked; -1 follows the loaded model. Loading takes a double-click, Enter or Load.
    int libraryCursor = -1;
    // Frame focus last sat on the clicked row; Enter needs it there on every frame since the click.
    int libraryPickFrame = -1;
    ImGuiID libraryPickId = 0;
    bool scrollLibraryToCurrent = true;

    std::array<float, gui_detail::kFrameTimeHistory> frameTimesMs{};
    std::size_t nextFrameTime = 0;
    std::size_t frameTimeCount = 0;

    std::string deviceName;
    std::uint32_t apiVersion = 0;
    bool cpuDevice = false;
    ImGuiTextFilter modelFilter;

    void resetLayout()
    {
        showScene = true;
        showInspector = true;
        showProfiler = true;
        sceneWidth = gui_detail::kDefaultSceneDockWidth;
        inspectorWidth = gui_detail::kDefaultInspectorDockWidth;
        profilerHeight = gui_detail::kDefaultProfilerDockHeight;
        userScale = 1.0F;
    }

    void recordFrameTime(float milliseconds)
    {
        frameTimesMs[nextFrameTime] = milliseconds;
        nextFrameTime = (nextFrameTime + 1) % frameTimesMs.size();
        frameTimeCount = std::min(frameTimeCount + 1, frameTimesMs.size());
    }
};
}// namespace Kataglyphis::Frontend

namespace {
using namespace Kataglyphis::Frontend::gui_detail;
using Kataglyphis::VulkanRendererInternals::FrontendShared::GPU_TIMED_PASS_NAMES;
using Kataglyphis::VulkanRendererInternals::FrontendShared::RasterizationMode;
using Kataglyphis::VulkanRendererInternals::FrontendShared::SceneViewport;

#ifdef NDEBUG
constexpr const char *kBuildType = "Release";
#else
constexpr const char *kBuildType = "Debug";
#endif

constexpr const char *kRepositoryUrl = "https://github.com/Kataglyphis/BeschleunigerBallett";

constexpr std::array<const char *, 3> kRenderModeLabels = { "Raster", "Ray traced", "Path traced" };
constexpr std::array<const char *, 2> kRasterPathLabels = { "Forward", "Deferred" };
constexpr std::array kUiScaleSteps = { 0.85F, 1.0F, 1.15F, 1.3F, 1.5F };
constexpr std::array<const char *, kUiScaleSteps.size()> kUiScaleLabels = { "85 %",
    "100 %",
    "115 %",
    "130 %",
    "150 %" };

// Segment labels for the small integer controls; both bounds come from host_device_shared_vars.hpp.
constexpr std::array<const char *, 9> kDigitLabels = { "0", "1", "2", "3", "4", "5", "6", "7", "8" };
static_assert(
  MAX_CASCADES < static_cast<int>(kDigitLabels.size()) && MAX_PCF_RADIUS < static_cast<int>(kDigitLabels.size()),
  "one digit label per cascade count and PCF radius");

struct TabInfo
{
    InspectorTab tab;
    const char *label;
    SceneObject primary;
};
constexpr std::array kInspectorTabs = {
    TabInfo{ InspectorTab::Model, "Model", SceneObject::Model },
    TabInfo{ InspectorTab::Sun, "Sun", SceneObject::Sun },
    TabInfo{ InspectorTab::Camera, "Camera", SceneObject::Camera },
    TabInfo{ InspectorTab::Sky, "Sky", SceneObject::Skybox },
    TabInfo{ InspectorTab::Renderer, "Renderer", SceneObject::Renderer },
};

struct ObjectInfo
{
    const char *name;
    StudioIcon icon;
    const char *kind;
};

constexpr auto objectInfo(SceneObject object) -> ObjectInfo
{
    switch (object) {
    case SceneObject::Model:
        return { "Model", StudioIcon::Cube, nullptr };
    case SceneObject::Camera:
        return { "Camera", StudioIcon::Camera, "Perspective" };
    case SceneObject::Skybox:
        return { "Skybox", StudioIcon::Mountain, "Cube map" };
    case SceneObject::Clouds:
        return { "Clouds", StudioIcon::Cloud, "Volumetric" };
    case SceneObject::Renderer:
        return { "Renderer", StudioIcon::Layers, nullptr };
    case SceneObject::Sun:
        break;
    }
    return { "Sun", StudioIcon::Sun, "Directional light" };
}

constexpr auto inspectorTabFor(SceneObject object) -> InspectorTab
{
    switch (object) {
    case SceneObject::Model:
        return InspectorTab::Model;
    case SceneObject::Camera:
        return InspectorTab::Camera;
    case SceneObject::Skybox:
    case SceneObject::Clouds:
        return InspectorTab::Sky;
    case SceneObject::Renderer:
        return InspectorTab::Renderer;
    case SceneObject::Sun:
        break;
    }
    return InspectorTab::Sun;
}

// Drops the shared Models/ prefix; a suffix of a std::string, so .data() stays null-terminated.
auto modelDisplayName(std::string_view name) -> std::string_view
{
    for (const std::string_view prefix : { std::string_view("Models/"), std::string_view("Models\\") }) {
        if (name.starts_with(prefix)) { return name.substr(prefix.size()); }
    }
    return name;
}

struct ModelPath
{
    std::string_view folder;
    std::string_view leaf;
};

// Splits at the last separator; scans on Windows mix backslashes into the display names.
auto splitModelPath(std::string_view name) -> ModelPath
{
    const std::size_t separator = name.find_last_of("/\\");
    if (separator == std::string_view::npos) { return { {}, name }; }
    return { name.substr(0, separator), name.substr(separator + 1) };
}

auto compareIgnoringCase(std::string_view lhs, std::string_view rhs) -> int
{
    const std::size_t common = std::min(lhs.size(), rhs.size());
    for (std::size_t index = 0; index < common; ++index) {
        const char left = ImToUpper(lhs[index]);
        const char right = ImToUpper(rhs[index]);
        if (left != right) { return (left < right) ? -1 : 1; }
    }
    if (lhs.size() == rhs.size()) { return 0; }
    return (lhs.size() < rhs.size()) ? -1 : 1;
}

auto libraryPath(std::span<const std::string> names, std::size_t index) -> ModelPath
{ return splitModelPath(modelDisplayName(names[index])); }

auto dockExtent(float preferred, float minimum, float maximumPixels, float scale) -> float
{
    const float lower = minimum * scale;
    return std::round(std::clamp(preferred * scale, lower, std::max(lower, maximumPixels)));
}

auto propertyLabelWidth() -> float
{
    const float fontSize = ImGui::GetFontSize();
    return std::round(std::clamp(ImGui::GetContentRegionAvail().x * 0.38F, fontSize * 5.5F, fontSize * 9.0F));
}

void mutedText(const char *text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, studio_palette::kTextMuted);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void mutedWrappedText(const char *text)
{
    ImGui::PushTextWrapPos(0.0F);
    mutedText(text);
    ImGui::PopTextWrapPos();
}

void formatApiVersion(std::span<char> out, std::uint32_t version)
{
    // VK_API_VERSION_MAJOR and friends are C-style-cast macros; the bit layout is fixed by the spec.
    std::snprintf(out.data(),
      out.size(),
      "Vulkan %u.%u.%u",
      static_cast<unsigned int>(version >> 22U),
      static_cast<unsigned int>((version >> 12U) & 0x3FFU),
      static_cast<unsigned int>(version & 0xFFFU));
}

// A caption on a frame-height line with a hairline under it; with `enabled`, a switch at its right end.
void captionLine(const char *caption, const char *toggleId = nullptr, bool *enabled = nullptr)
{
    const ImGuiStyle &style = ImGui::GetStyle();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetFrameHeight();
    studioCaption(caption, height);
    if (enabled != nullptr) {
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(origin.x + width - studioToggleWidth(), origin.y));
        studioToggle(toggleId, enabled);
    }
    const float rule = std::max(1.0F, style.SeparatorSize);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(origin.x, origin.y + height - rule),
      ImVec2(origin.x + width, origin.y + height),
      ImGui::GetColorU32(ImGuiCol_Separator));
}

void sectionHeader(const char *caption, const char *toggleId = nullptr, bool *enabled = nullptr)
{
    ImGui::Dummy(ImVec2(0.0F, ImGui::GetStyle().ItemSpacing.y * 0.5F));
    captionLine(caption, toggleId, enabled);
}

void drawEllipsized(ImDrawList *drawList, ImVec2 pos, float maxX, std::string_view text, ImVec4 color)
{
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::RenderTextEllipsis(drawList,
      pos,
      ImVec2(maxX, pos.y + ImGui::GetTextLineHeight()),
      maxX,
      text.data(),
      text.data() + text.size(),
      nullptr);
    ImGui::PopStyleColor();
}

struct RowSpec
{
    std::optional<StudioIcon> icon;
    std::string_view name;
    std::string_view detail;
    bool highlighted = false;
    bool marked = false;
    bool *visible = nullptr;
    const char *visibilityTooltip = nullptr;
    const char *action = nullptr;
    const char *tooltip = nullptr;
    ImGuiSelectableFlags flags = ImGuiSelectableFlags_None;
};

struct RowResult
{
    bool clicked = false;
    bool activated = false;
    bool action = false;
    ImGuiID id = 0;
};

// One line of the outliner or the library: `marked` draws the mint bar, `highlighted` the row fill.
auto listRow(const char *strId, const RowSpec &spec) -> RowResult
{
    RowResult result;
    ImGui::PushID(strId);
    const ImGuiStyle &style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight();
    const float width = ImGui::GetContentRegionAvail().x;
    const float fontSize = ImGui::GetFontSize();
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    ImGuiSelectableFlags flags = ImGuiSelectableFlags_AllowOverlap | spec.flags;
    flags |= ImGuiSelectableFlags_NoPadWithHalfSpacing;
    result.clicked = ImGui::Selectable("##row", spec.highlighted, flags, ImVec2(width, height));
    result.id = ImGui::GetItemID();
    result.activated =
      result.clicked
      && (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)
          || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
    if (spec.tooltip != nullptr) { ImGui::SetItemTooltip("%s", spec.tooltip); }

    ImDrawList *drawList = ImGui::GetWindowDrawList();
    if (spec.marked) {
        drawList->AddRectFilled(origin,
          ImVec2(origin.x + std::max(2.0F, style.TabBarOverlineSize), origin.y + height),
          ImGui::GetColorU32(studio_palette::kAccent));
    }
    const bool shown = (spec.visible == nullptr) || *spec.visible;
    const float textY = origin.y + std::round((height - ImGui::GetTextLineHeight()) * 0.5F);
    float textX = origin.x + style.FramePadding.x;
    if (spec.icon.has_value()) {
        const ImVec4 iconColor =
          !shown ? studio_palette::kTextDisabled : (spec.marked ? studio_palette::kText : studio_palette::kTextMuted);
        drawStudioIcon(drawList,
          *spec.icon,
          ImVec2(textX, origin.y + std::round((height - fontSize) * 0.5F)),
          fontSize,
          ImGui::GetColorU32(iconColor));
        textX += fontSize + style.ItemInnerSpacing.x;
    }

    float contentRight = origin.x + width - style.FramePadding.x;
    if (spec.visible != nullptr) {
        // Placed so the eye's ink ends where a detail would end.
        const float buttonX = contentRight - std::round((height + fontSize) * 0.5F);
        ImGui::SetCursorScreenPos(ImVec2(buttonX, origin.y));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0F, 0.0F, 0.0F, 0.0F));
        if (studioIconButton(
              "##visible", *spec.visible ? StudioIcon::Eye : StudioIcon::EyeOff, spec.visibilityTooltip)) {
            *spec.visible = !*spec.visible;
        }
        ImGui::PopStyleColor();
        contentRight = buttonX + std::round((height - fontSize) * 0.5F) - style.ItemInnerSpacing.x;
    } else if (spec.action != nullptr) {
        // Row height, so the next row keeps its pitch without moving the cursor by hand.
        const float buttonWidth = ImGui::CalcTextSize(spec.action).x + style.FramePadding.x * 2.0F;
        const float buttonX = origin.x + width - buttonWidth;
        ImGui::SetCursorScreenPos(ImVec2(buttonX, origin.y));
        result.action = ImGui::Button(spec.action, ImVec2(buttonWidth, height));
        contentRight = buttonX - style.ItemInnerSpacing.x;
    }

    // The name keeps priority; the detail takes what is left and ellipsizes first.
    const float available = std::max(0.0F, contentRight - textX);
    const float gap = style.ItemSpacing.x;
    const float nameWidth = ImGui::CalcTextSize(spec.name.data(), spec.name.data() + spec.name.size()).x;
    float detailSpace = 0.0F;
    if (!spec.detail.empty()) {
        const float detailWidth = ImGui::CalcTextSize(spec.detail.data(), spec.detail.data() + spec.detail.size()).x;
        detailSpace = std::min(detailWidth, std::max(0.0F, available - std::min(nameWidth, available * 0.6F) - gap));
    }
    const float nameMax = textX + std::max(0.0F, available - ((detailSpace > 0.0F) ? detailSpace + gap : 0.0F));
    drawEllipsized(drawList,
      ImVec2(textX, textY),
      nameMax,
      spec.name,
      shown ? studio_palette::kText : studio_palette::kTextDisabled);
    if (detailSpace > 0.0F) {
        drawEllipsized(
          drawList, ImVec2(contentRight - detailSpace, textY), contentRight, spec.detail, studio_palette::kTextMuted);
    }
    ImGui::PopID();
    return result;
}

// The inspector's title row: object icon and name in the title size, then its kind in the muted body face.
void inspectorTitle(const ObjectInfo &info, std::string_view subtitle)
{
    ImGui::Dummy(ImVec2(0.0F, ImGui::GetStyle().ItemSpacing.y * 0.5F));
    ImGui::PushFont(nullptr, kKataglyphisUiFontPixels * kStudioTitleScale);
    const float title_ascent = ImGui::GetFontBaked()->Ascent;
    studioIconText(info.icon, ImGui::GetColorU32(studio_palette::kTextMuted));
    ImGui::SameLine();
    ImGui::TextUnformatted(info.name);
    ImGui::PopFont();
    if (subtitle.empty()) { return; }
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::round(title_ascent - ImGui::GetFontBaked()->Ascent));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float maxX = pos.x + ImGui::GetContentRegionAvail().x;
    drawEllipsized(ImGui::GetWindowDrawList(), pos, maxX, subtitle, studio_palette::kTextMuted);
    ImGui::Dummy(ImVec2(std::max(0.0F, maxX - pos.x), ImGui::GetTextLineHeight()));
}

void countRow(const char *name, unsigned int drawn, unsigned int total)
{
    std::array<char, 48> value{};
    std::snprintf(value.data(), value.size(), "%u / %u", drawn, total);
    const float fraction = (total > 0U) ? static_cast<float>(drawn) / static_cast<float>(total) : 0.0F;
    studioMetricRow(name, fraction, value.data(), total > 0U);
}

auto culledPercent(unsigned int drawn, unsigned int total) -> double
{ return (total > 0U) ? 100.0 * static_cast<double>(total - drawn) / static_cast<double>(total) : 0.0; }
}// namespace

GUI::GUI(Window *window) : window(window), studio(std::make_unique<StudioState>()) {}

void GUI::initializeVulkanContext(const std::shared_ptr<VulkanDevice> &vulkan_device,
  const vk::Instance &instance,
  vk::Format post_color_format,
  uint32_t image_count)
{
    this->device = vulkan_device;

    create_gui_context(window, instance, post_color_format, image_count);
}

void GUI::setUserSelectionForRRT(bool rrtCapabilitiesAvailable)
{ renderUserSelectionForRRT = rrtCapabilitiesAvailable; }

int GUI::renderMode() const
{ return guiRendererSharedVars.pathTracing ? 2 : (guiRendererSharedVars.raytracing ? 1 : 0); }

void GUI::setRenderMode(int mode)
{
    guiRendererSharedVars.raytracing = (mode == 1);
    guiRendererSharedVars.pathTracing = (mode == 2);
}

bool GUI::tracingAvailable() const { return renderUserSelectionForRRT && guiRendererSharedVars.tracingReady; }

int GUI::displayedRenderMode() const
{
    return Kataglyphis::VulkanRendererInternals::FrontendShared::drawnRenderMode(
      guiRendererSharedVars.raytracing, guiRendererSharedVars.pathTracing, tracingAvailable());
}

void GUI::renderModeSwitch(const char *id, float width)
{
    int mode = displayedRenderMode();
    const std::array<bool, 3> available = { true, tracingAvailable(), tracingAvailable() };
    // Reselectable: it shows Raster while a tracing pick waits for a TLAS, and a press on Raster drops that pick.
    if (studioSegmented(id, &mode, kRenderModeLabels, available, width, true)) { setRenderMode(mode); }
}

auto GUI::loadedModelName() const -> std::string_view
{
    const auto model_names = sceneConfig::getAvailableModelDisplayNames();
    const int loaded = guiSceneSharedVars.selected_model_index;
    if (loaded >= 0 && loaded < static_cast<int>(model_names.size())) {
        return modelDisplayName(model_names[static_cast<std::size_t>(loaded)]);
    }
    // A model the library does not list, such as an absolute KATAGLYPHIS_MODEL_OVERRIDE, goes by its file's stem.
    const std::string_view path = guiRendererSharedVars.loadedModelPath;
    const std::size_t slash = path.find_last_of("/\\");
    const std::string_view file = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = file.find_last_of('.');
    return (dot == std::string_view::npos || dot == 0) ? file : file.substr(0, dot);
}

void GUI::render()
{
    // The style may only change between frames, so following the monitor's DPI happens before NewFrame().
    const float content_scale = ImGui_ImplGlfw_GetContentScaleForWindow(window->get_window());
    const float ui_scale = std::clamp(content_scale * studio->userScale, 0.5F, 4.0F);
    if (std::abs(ui_scale - studio->appliedScale) > 0.001F) {
        applyKataglyphisImGuiDarkTheme(ui_scale);
        studio->appliedScale = ui_scale;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    // A click's down, up and down reach ImGui one frame each, so at llvmpipe frame rates 0.3 s is never a double-click.
    ImGuiIO &frame_io = ImGui::GetIO();
    frame_io.MouseDoubleClickTime = std::clamp(1.5F * (studio->previousDeltaTime + frame_io.DeltaTime), 0.3F, 2.0F);
    // NewFrame asserts this stays above the double-click window; at 60 FPS both are ImGui's defaults again.
    frame_io.MouseSingleClickDelay = frame_io.MouseDoubleClickTime + 0.2F;
    studio->previousDeltaTime = frame_io.DeltaTime;
    ImGui::NewFrame();

    const ImGuiIO &io = ImGui::GetIO();
    studio->recordFrameTime(io.DeltaTime * 1000.0F);
    // Checked at frame start too: a menu-bar click takes focus at EndFrame and the bar returns it before the rows.
    if (ImGui::GetFocusID() != studio->libraryPickId) { studio->libraryPickFrame = -1; }

    // Read before the hidden early-out, or F1 could hide the editor but never bring it back.
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
        overlayVisible = !overlayVisible;
        studio->releaseFocus = overlayVisible;
        studio->scrollLibraryToCurrent = overlayVisible;
    }
#ifndef NDEBUG
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        guiRendererSharedVars.shader_hot_reload_triggered = true;
    }
#endif

    // Re-derived from the shared vars every frame, never cached, so writes from code are not stomped.
    setRenderMode(renderMode());

    // A minimized window has no area: the bars and docks would auto-fit to their content instead.
    const ImGuiViewport *main_viewport = ImGui::GetMainViewport();
    if (!overlayVisible || main_viewport->Size.x <= 0.0F || main_viewport->Size.y <= 0.0F) {
        guiRendererSharedVars.scene_viewport = SceneViewport{};
        // The bars take focus again when they reappear, after a restore as after F1.
        studio->releaseFocus = true;
        // Without docks no splitter sees the release, so a drag would resume once they are back.
        studio->sceneDrag.active = false;
        studio->inspectorDrag.active = false;
        studio->profilerDrag.active = false;
        return;
    }

    renderMenuBar();
    renderStatusBar();
    renderDocks(ui_scale);
    renderFloatingWindows();

    // The menu bar takes focus when it appears, which would make the first Esc only leave it.
    if (studio->releaseFocus) {
        ImGui::SetWindowFocus(nullptr);
        studio->releaseFocus = false;
    }
    // The first stage of the two-stage Esc (WindowInputCallbacks quits on the second); popups close themselves.
    const bool popup_open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup);
    if (!io.WantTextInput && !popup_open && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::SetWindowFocus(nullptr);
    }
    // ImGui only drops focus on a left click into the scene; right-dragging to look around leaves the panel too.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !io.WantCaptureMouse) { ImGui::SetWindowFocus(nullptr); }
}

void GUI::renderDocks(float uiScale)
{
    const ImRect inner = studioSceneRect();
    const float min_view_width = kMinSceneViewWidth * uiScale;
    const float min_view_height = kMinSceneViewHeight * uiScale;
    const float scene_dock =
      dockExtent(studio->sceneWidth, kMinSceneDockWidth, inner.GetWidth() * kMaxSceneDockShare, uiScale);
    const float inspector_dock =
      dockExtent(studio->inspectorWidth, kMinInspectorDockWidth, inner.GetWidth() * kMaxInspectorDockShare, uiScale);
    const float profiler_dock =
      dockExtent(studio->profilerHeight, kMinProfilerDockHeight, inner.GetHeight() * kMaxProfilerDockShare, uiScale);

    // Extents are whole pixels, so a dock dragged to its splitter's stop may round half a pixel past the limit.
    const float room_x = inner.GetWidth() + 0.5F;
    const float room_y = inner.GetHeight() + 0.5F;
    // A narrow window drops the scene dock first, then the inspector; a low one drops the profiler.
    const bool show_inspector = studio->showInspector && inspector_dock + min_view_width <= room_x;
    const float inspector_share = show_inspector ? inspector_dock : 0.0F;
    const bool show_scene = studio->showScene && scene_dock + inspector_share + min_view_width <= room_x;
    const float scene_share = show_scene ? scene_dock : 0.0F;
    const bool show_profiler = studio->showProfiler && profiler_dock + min_view_height <= room_y;
    if (show_scene) { renderSceneDock(scene_dock); }
    if (show_inspector) { renderInspectorDock(inspector_dock); }
    if (show_profiler) { renderProfilerDock(profiler_dock); }

    const ImRect scene = studioSceneRect();
    const float handle = std::round(kSplitterThickness * uiScale);
    // A dock dropped mid-drag never sees the release, so its drag would resume with another splitter's.
    studio->sceneDrag.active = studio->sceneDrag.active && show_scene;
    studio->inspectorDrag.active = studio->inspectorDrag.active && show_inspector;
    studio->profilerDrag.active = studio->profilerDrag.active && show_profiler;
    if (show_scene) {
        float width = scene_dock;
        if (studioDockSplitter(ImRect(scene.Min.x, scene.Min.y, scene.Min.x + handle, scene.Max.y),
              ImGuiAxis_X,
              1.0F,
              &width,
              kMinSceneDockWidth * uiScale,
              std::min(inner.GetWidth() * kMaxSceneDockShare, inner.GetWidth() - inspector_share - min_view_width),
              &studio->sceneDrag)) {
            studio->sceneWidth = width / uiScale;
        }
    }
    if (show_inspector) {
        float width = inspector_dock;
        if (studioDockSplitter(ImRect(scene.Max.x - handle, scene.Min.y, scene.Max.x, scene.Max.y),
              ImGuiAxis_X,
              -1.0F,
              &width,
              kMinInspectorDockWidth * uiScale,
              std::min(inner.GetWidth() * kMaxInspectorDockShare, inner.GetWidth() - scene_share - min_view_width),
              &studio->inspectorDrag)) {
            studio->inspectorWidth = width / uiScale;
        }
    }
    if (show_profiler) {
        float height = profiler_dock;
        if (studioDockSplitter(ImRect(scene.Min.x + handle, scene.Max.y - handle, scene.Max.x - handle, scene.Max.y),
              ImGuiAxis_Y,
              -1.0F,
              &height,
              kMinProfilerDockHeight * uiScale,
              std::min(inner.GetHeight() * kMaxProfilerDockShare, inner.GetHeight() - min_view_height),
              &studio->profilerDrag)) {
            studio->profilerHeight = height / uiScale;
        }
    }

    // The renderer centres the projection on this rect; the docks cover the rest of the image.
    const ImGuiViewport *main_viewport = ImGui::GetMainViewport();
    guiRendererSharedVars.scene_viewport =
      SceneViewport{ .x0 = (scene.Min.x - main_viewport->Pos.x) / main_viewport->Size.x,
          .y0 = (scene.Min.y - main_viewport->Pos.y) / main_viewport->Size.y,
          .x1 = (scene.Max.x - main_viewport->Pos.x) / main_viewport->Size.x,
          .y1 = (scene.Max.y - main_viewport->Pos.y) / main_viewport->Size.y };
    renderViewportOverlay(scene.Min.x, scene.Min.y, scene.Max.x, scene.Max.y);
}

void GUI::renderMenuBar()
{
    const ImGuiStyle &style = ImGui::GetStyle();
    const ImGuiIO &io = ImGui::GetIO();
    // A taller bar than the frame height, so the renderer switch sits in it with a margin.
    const ImVec2 bar_padding(style.FramePadding.x, style.FramePadding.y + std::round(3.0F * studio->appliedScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, bar_padding);
    const bool open = ImGui::BeginMainMenuBar();
    ImGui::PopStyleVar(2);
    if (!open) { return; }

    studioBarHairline(ImGuiDir_Up);
    ImGuiWindow *bar = ImGui::GetCurrentWindow();
    const float bar_top = bar->Pos.y;
    const float bar_height = bar->Size.y;
    const float font_size = ImGui::GetFontSize();

    // The brand mark stays neutral: mint is for selection and live values.
    const ImVec2 brand = ImGui::GetCursorScreenPos();
    drawStudioIcon(bar->DrawList,
      StudioIcon::Cube,
      ImVec2(brand.x, bar_top + std::round((bar_height - font_size) * 0.5F)),
      font_size,
      ImGui::GetColorU32(studio_palette::kTextMuted));
    ImGui::Dummy(ImVec2(font_size, font_size));
    ImGui::TextUnformatted("BeschleunigerBallett");
    const float divider = ImGui::GetCursorScreenPos().x;
    bar->DrawList->AddRectFilled(ImVec2(divider, bar_top + std::round((bar_height - font_size) * 0.5F)),
      ImVec2(divider + std::max(1.0F, style.SeparatorSize), bar_top + std::round((bar_height + font_size) * 0.5F)),
      ImGui::GetColorU32(ImGuiCol_Border));
    ImGui::Dummy(ImVec2(std::max(1.0F, style.SeparatorSize), 0.0F));

    if (ImGui::BeginMenu("File")) {
#ifndef NDEBUG
        if (ImGui::MenuItem("Reload shaders", "F5")) { guiRendererSharedVars.shader_hot_reload_triggered = true; }
#else
        ImGui::MenuItem("Reload shaders", "F5", false, false);
        ImGui::SetItemTooltip("Debug builds only");
#endif
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Esc")) { glfwSetWindowShouldClose(window->get_window(), GLFW_TRUE); }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Scene", nullptr, &studio->showScene);
        ImGui::MenuItem("Inspector", nullptr, &studio->showInspector);
        ImGui::MenuItem("Profiler", nullptr, &studio->showProfiler);
        if (ImGui::MenuItem("Hide editor", "F1")) { overlayVisible = false; }
        ImGui::Separator();
        if (ImGui::BeginMenu("UI scale")) {
            for (std::size_t step = 0; step < kUiScaleSteps.size(); ++step) {
                const bool current = std::abs(studio->userScale - kUiScaleSteps[step]) < 0.01F;
                if (ImGui::MenuItem(kUiScaleLabels[step], nullptr, current)) {
                    studio->userScale = kUiScaleSteps[step];
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Reset layout")) { studio->resetLayout(); }
        ImGui::Separator();
        ImGui::MenuItem("Style editor", nullptr, &studio->showStyleEditor);
        ImGui::MenuItem("Dear ImGui metrics", nullptr, &studio->showMetrics);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Render")) {
        const int mode = displayedRenderMode();
        for (std::size_t index = 0; index < kRenderModeLabels.size(); ++index) {
            const int option = static_cast<int>(index);
            if (ImGui::MenuItem(kRenderModeLabels[index], nullptr, mode == option, option == 0 || tracingAvailable())) {
                setRenderMode(option);
            }
        }
        ImGui::Separator();
        const bool deferred = guiRendererSharedVars.rasterizationMode == RasterizationMode::Deferred;
        if (ImGui::MenuItem("Forward", nullptr, !deferred, mode == 0)) {
            guiRendererSharedVars.rasterizationMode = RasterizationMode::Forward;
        }
        if (ImGui::MenuItem("Deferred", nullptr, deferred, mode == 0)) {
            guiRendererSharedVars.rasterizationMode = RasterizationMode::Deferred;
        }
        ImGui::Separator();
        ImGui::MenuItem("Frustum culling", nullptr, &guiRendererSharedVars.frustum_culling_enabled);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Key bindings")) {
            studio->selection = SceneObject::Camera;
            studio->selectionChanged = true;
            studio->showInspector = true;
        }
        if (ImGui::MenuItem("About")) { studio->openAbout = true; }
        ImGui::EndMenu();
    }

    // Centred renderer switch, dropped when the window is too narrow to keep it clear of the menus.
    const float after_menus = ImGui::GetCursorScreenPos().x;
    float widest = 0.0F;
    for (const char *label : kRenderModeLabels) { widest = std::max(widest, ImGui::CalcTextSize(label).x); }
    const float switch_width =
      std::round((widest + style.FramePadding.x * 3.0F) * static_cast<float>(kRenderModeLabels.size()));
    const float switch_x = std::round(bar->Pos.x + (bar->Size.x - switch_width) * 0.5F);
    float occupied_right = after_menus;
    if (switch_x > after_menus + style.ItemSpacing.x) {
        ImGui::SetCursorScreenPos(
          ImVec2(switch_x, bar_top + std::round((bar_height - ImGui::GetFrameHeight()) * 0.5F)));
        renderModeSwitch("##renderer_switch", switch_width);
        occupied_right = switch_x + switch_width;
    }

    std::array<char, 48> fps{};
    const double frame_ms = (io.Framerate > 0.0F) ? 1000.0 / static_cast<double>(io.Framerate) : 0.0;
    std::snprintf(fps.data(), fps.size(), "%.0f FPS  \xC2\xB7  %.2f ms", static_cast<double>(io.Framerate), frame_ms);
    const float fps_x = bar->Pos.x + bar->Size.x - style.WindowPadding.x - ImGui::CalcTextSize(fps.data()).x;
    if (fps_x > occupied_right + style.ItemSpacing.x) {
        ImGui::SetCursorScreenPos(ImVec2(fps_x, bar_top));
        mutedText(fps.data());
    }

    ImGui::EndMainMenuBar();
}

void GUI::renderStatusBar()
{
    constexpr ImGuiWindowFlags kStatusBarFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings
                                                 | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoFocusOnAppearing
                                                 | ImGuiWindowFlags_NoNav;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    const bool open = ImGui::BeginViewportSideBar(
      "##studio_status_bar", ImGui::GetMainViewport(), ImGuiDir_Down, ImGui::GetFrameHeight(), kStatusBarFlags);
    ImGui::PopStyleVar();
    if (open && ImGui::BeginMenuBar()) {
        studioBarHairline(ImGuiDir_Down);
        const ImGuiIO &io = ImGui::GetIO();
        const ImGuiStyle &style = ImGui::GetStyle();
        ImGuiWindow *bar = ImGui::GetCurrentWindow();

        // Amber for a CPU rasterizer such as llvmpipe, whose frame rates say nothing about the engine.
        studioStatusDot(studio->cpuDevice ? studio_palette::kWarning : studio_palette::kAccent);
        std::array<char, 32> api{};
        formatApiVersion(api, studio->apiVersion);
        std::array<char, 320> left{};
        std::snprintf(left.data(), left.size(), "%s  \xC2\xB7  %s", api.data(), studio->deviceName.c_str());
        mutedText(left.data());
        if (studio->cpuDevice) {
            ImGui::SetItemTooltip("CPU Vulkan device: software rendering, expect low frame rates");
        }

        std::array<char, 160> right{};
        std::snprintf(right.data(),
          right.size(),
          "%d \xC3\x97 %d  \xC2\xB7  %s  \xC2\xB7  v" PROJECT_VERSION "  \xC2\xB7  F1 hides the editor",
          static_cast<int>(io.DisplaySize.x * io.DisplayFramebufferScale.x),
          static_cast<int>(io.DisplaySize.y * io.DisplayFramebufferScale.y),
          kBuildType);
        const float right_x = bar->Pos.x + bar->Size.x - style.WindowPadding.x - ImGui::CalcTextSize(right.data()).x;
        if (right_x > ImGui::GetCursorScreenPos().x + style.ItemSpacing.x) {
            ImGui::SetCursorScreenPos(ImVec2(right_x, bar->Pos.y));
            mutedText(right.data());
        }
        ImGui::EndMenuBar();
    }
    ImGui::End();
}

void GUI::renderSceneDock(float width)
{
    if (beginStudioDock(
          "##studio_scene", ImGuiDir_Left, width, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        captionLine("SCENE");

        const std::string_view loaded_name = loadedModelName();
        const std::string_view model_name = loaded_name.empty() ? std::string_view("none") : loaded_name;

        std::array<char, 24> sun{};
        if (guiSceneSharedVars.shadows_enabled) {
            std::snprintf(sun.data(), sun.size(), "%d cascades", guiSceneSharedVars.num_shadow_cascades);
        } else {
            std::snprintf(sun.data(), sun.size(), "no shadows");
        }
        std::array<char, 16> fov{};
        std::snprintf(fov.data(), fov.size(), "%.0f\xC2\xB0", static_cast<double>(guiSceneSharedVars.camera_fov));

        const auto row = [this](SceneObject object, std::string_view detail, bool *visible, const char *tooltip) {
            const ObjectInfo info = objectInfo(object);
            RowSpec spec;
            spec.icon = info.icon;
            spec.name = info.name;
            spec.detail = detail;
            spec.highlighted = studio->selection == object;
            spec.marked = spec.highlighted;
            spec.visible = visible;
            spec.visibilityTooltip = tooltip;
            if (listRow(info.name, spec).clicked) {
                studio->selection = object;
                studio->selectionChanged = true;
                studio->showInspector = true;
            }
        };
        // A tighter pitch than the property grid, so the highlights of adjacent rows nearly touch.
        ImGui::PushStyleVarY(ImGuiStyleVar_ItemSpacing, std::round(ImGui::GetStyle().ItemSpacing.y / 3.0F));
        row(SceneObject::Model, model_name, nullptr, nullptr);
        row(SceneObject::Sun, sun.data(), nullptr, nullptr);
        row(SceneObject::Camera, fov.data(), nullptr, nullptr);
        row(SceneObject::Skybox, {}, &guiSceneSharedVars.skybox_enabled, "Show or hide the skybox");
        row(SceneObject::Clouds, {}, &guiSceneSharedVars.clouds_enabled, "Show or hide the clouds");
        row(
          SceneObject::Renderer, kRenderModeLabels[static_cast<std::size_t>(displayedRenderMode())], nullptr, nullptr);
        ImGui::PopStyleVar();

        sectionHeader("LIBRARY");
        renderModelLibrary();
    }
    endStudioDock();
}

void GUI::renderModelLibrary()
{
    const auto model_names = sceneConfig::getAvailableModelDisplayNames();
    const auto model_paths = sceneConfig::getAvailableModelPaths();
    if (model_names.empty() || model_names.size() != model_paths.size()) {
        mutedWrappedText("No loadable models (.obj/.gltf/.glb) found in Resources/Models/");
        return;
    }

    // Folders first, then loose files, each by name; the indices stay sceneConfig's, which the renderer reads.
    if (studio->libraryOrder.size() != model_names.size()) {
        studio->libraryOrder.resize(model_names.size());
        std::iota(studio->libraryOrder.begin(), studio->libraryOrder.end(), std::size_t{ 0 });
        std::sort(
          studio->libraryOrder.begin(), studio->libraryOrder.end(), [&model_names](std::size_t lhs, std::size_t rhs) {
              const ModelPath left = libraryPath(model_names, lhs);
              const ModelPath right = libraryPath(model_names, rhs);
              if (left.folder.empty() != right.folder.empty()) { return right.folder.empty(); }
              const int folder = compareIgnoringCase(left.folder, right.folder);
              if (folder != 0) { return folder < 0; }
              return compareIgnoringCase(left.leaf, right.leaf) < 0;
          });
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint(
          "##model_filter", "Filter", studio->modelFilter.InputBuf, sizeof(studio->modelFilter.InputBuf))) {
        studio->modelFilter.Build();
    }
    const bool filtering = studio->modelFilter.IsActive();
    const int loaded = guiSceneSharedVars.selected_model_index;
    const int cursor = (studio->libraryCursor >= 0) ? studio->libraryCursor : loaded;

    std::array<char, 640> tooltip{};
    bool load_requested = false;
    ImGuiID picked_id = 0;
    const auto request_load = [&](int row) {
        guiSceneSharedVars.selected_model_index = row;
        guiSceneSharedVars.model_reload_requested = true;
        studio->libraryCursor = row;
        load_requested = true;
    };
    const auto library_row = [&](std::size_t index, std::string_view name, std::string_view detail) {
        const int row = static_cast<int>(index);
        RowSpec spec;
        spec.name = name;
        spec.detail = detail;
        spec.highlighted = row == cursor;
        spec.marked = row == loaded;
        spec.action = (spec.highlighted && !spec.marked) ? "Load" : nullptr;
        spec.flags = ImGuiSelectableFlags_AllowDoubleClick;
        // Enter loads the focused pick, not the hovered row, so only the double-click is promised here.
        std::snprintf(tooltip.data(),
          tooltip.size(),
          "%s%s",
          model_paths[index].c_str(),
          spec.marked ? "" : "\nDouble-click to load");
        spec.tooltip = tooltip.data();
        const bool was_pick = row == studio->libraryCursor;
        ImGui::PushID(row);
        const RowResult result = listRow("##model", spec);
        ImGui::PopID();
        if (spec.marked && studio->scrollLibraryToCurrent) {
            ImGui::SetScrollHereY(0.35F);
            studio->scrollLibraryToCurrent = false;
        }
        if (result.clicked) {
            studio->libraryCursor = row;
            studio->libraryPickFrame = ImGui::GetFrameCount();
        }
        // ImGui pairs quick clicks on neighbouring rows into a double-click, so only the picked row loads that way.
        const bool double_click = result.activated && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        // A load stalls the GPU: a click only picks; no reload of the loaded row, or a Load double-click loads twice.
        if (((result.activated && (!double_click || was_pick)) || result.action) && !spec.marked) { request_load(row); }
        if (row == studio->libraryCursor) { picked_id = result.id; }
    };

    if (ImGui::BeginChild("##model_library", ImVec2(0.0F, 0.0F), ImGuiChildFlags_NavFlattened)) {
        ImGui::PushStyleVarY(ImGuiStyleVar_ItemSpacing, std::round(ImGui::GetStyle().ItemSpacing.y / 3.0F));
        const std::vector<std::size_t> &order = studio->libraryOrder;
        std::vector<std::size_t> rows;
        std::size_t first = 0;
        while (first < order.size()) {
            const ModelPath head = libraryPath(model_names, order[first]);
            std::size_t last = first + 1;
            while (last < order.size()
                   && compareIgnoringCase(libraryPath(model_names, order[last]).folder, head.folder) == 0) {
                ++last;
            }
            rows.clear();
            for (std::size_t position = first; position < last; ++position) {
                const std::string_view name = modelDisplayName(model_names[order[position]]);
                if (!filtering || studio->modelFilter.PassFilter(name.data(), name.data() + name.size())) {
                    rows.push_back(order[position]);
                }
            }
            const std::size_t group_size = last - first;
            first = last;
            if (rows.empty()) { continue; }

            if (head.folder.empty()) {
                for (const std::size_t index : rows) { library_row(index, libraryPath(model_names, index).leaf, {}); }
                continue;
            }
            // A folder with one model is one row, named by the folder, which is the scene's name.
            if (group_size == 1) {
                library_row(rows.front(), head.folder, head.leaf);
                continue;
            }
            const bool holds_loaded = std::any_of(
              rows.begin(), rows.end(), [loaded](std::size_t index) { return static_cast<int>(index) == loaded; });
            ImGui::PushID(head.folder.data(), head.folder.data() + head.folder.size());
            if (filtering) {
                ImGui::SetNextItemOpen(true);
            } else if (holds_loaded) {
                ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
            }
            ImGui::AlignTextToFramePadding();
            const bool open = ImGui::TreeNodeEx("##folder",
              ImGuiTreeNodeFlags_SpanAvailWidth,
              "%.*s",
              static_cast<int>(head.folder.size()),
              head.folder.data());
            std::array<char, 16> count{};
            std::snprintf(count.data(), count.size(), "%d", static_cast<int>(rows.size()));
            const ImVec2 node_min = ImGui::GetItemRectMin();
            const ImVec2 node_max = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddText(
              ImVec2(node_max.x - ImGui::GetStyle().FramePadding.x - ImGui::CalcTextSize(count.data()).x,
                node_min.y + std::round((node_max.y - node_min.y - ImGui::GetTextLineHeight()) * 0.5F)),
              ImGui::GetColorU32(studio_palette::kTextMuted),
              count.data());
            if (open) {
                for (const std::size_t index : rows) { library_row(index, libraryPath(model_names, index).leaf, {}); }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::PopStyleVar();

        // ImGui hands focus back to the row after a menu; that must not re-arm Enter once focus has left the row.
        const int frame = ImGui::GetFrameCount();
        const bool held = studio->libraryPickFrame >= frame - 1 && picked_id != 0 && ImGui::GetFocusID() == picked_id;
        studio->libraryPickFrame = held ? frame : -1;
        studio->libraryPickId = held ? picked_id : 0;
        // A click hides the nav cursor and ImGui's Enter needs a visible one, so Enter on the clicked row loads here.
        const bool enter =
          ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
        if (enter && held && !load_requested && !ImGui::GetCurrentContext()->NavCursorVisible
            && studio->libraryCursor != loaded) {
            request_load(studio->libraryCursor);
        }
    }
    ImGui::EndChild();
}

void GUI::renderInspectorDock(float width)
{
    if (beginStudioDock("##studio_inspector",
          ImGuiDir_Right,
          width,
          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const InspectorTab wanted = inspectorTabFor(studio->selection);
        const bool forced = studio->selectionChanged;
        const auto subtitle = [this](SceneObject subject) -> std::string_view {
            if (subject == SceneObject::Model) {
                const std::string_view loaded_name = loadedModelName();
                return loaded_name.empty() ? std::string_view("No model") : loaded_name;
            }
            if (subject == SceneObject::Renderer) {
                return kRenderModeLabels[static_cast<std::size_t>(displayedRenderMode())];
            }
            const char *kind = objectInfo(subject).kind;
            return (kind != nullptr) ? std::string_view(kind) : std::string_view();
        };

        if (ImGui::BeginTabBar("##inspector_tabs", ImGuiTabBarFlags_FittingPolicyShrink)) {
            for (const TabInfo &info : kInspectorTabs) {
                const ImGuiTabItemFlags flags =
                  (forced && info.tab == wanted) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                if (!studioTabItem(info.label, flags)) { continue; }
                // A click on a tab lands one frame later; a forced switch shows the old tab for that frame.
                if (!forced && info.tab != wanted) { studio->selection = info.primary; }
                const SceneObject subject =
                  (inspectorTabFor(studio->selection) == info.tab) ? studio->selection : info.primary;
                inspectorTitle(objectInfo(subject), subtitle(subject));
                if (ImGui::BeginChild("##inspector_body", ImVec2(0.0F, 0.0F), ImGuiChildFlags_NavFlattened)) {
                    switch (info.tab) {
                    case InspectorTab::Model:
                        renderModelInspector();
                        break;
                    case InspectorTab::Sun:
                        renderLightInspector();
                        break;
                    case InspectorTab::Camera:
                        renderCameraInspector();
                        break;
                    case InspectorTab::Sky:
                        renderSkyInspector();
                        break;
                    case InspectorTab::Renderer:
                        renderRenderInspector();
                        break;
                    }
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        studio->selectionChanged = false;
    }
    endStudioDock();
}

void GUI::renderLightInspector()
{
    if (beginStudioProperties("##sun", propertyLabelWidth())) {
        studioPropertyLabel("Radiance");
        studioSlider("##radiance", &guiSceneSharedVars.directional_light_radiance, 0.0F, 50.0F, "%.1f");
        studioPropertyLabel("Color", "Click the swatch for the picker");
        ImGui::ColorEdit3("##color",
          guiSceneSharedVars.directional_light_color,
          ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayHex);
        studioPropertyLabel("Direction", "Points from the sun into the scene; normalized before use");
        studioDragAxes("##direction",
          guiSceneSharedVars.directional_light_direction,
          0.005F,
          -1.0F,
          1.0F,
          "%.2f",
          ImGuiSliderFlags_AlwaysClamp);
        endStudioProperties();
    }

    sectionHeader("SHADOWS", "##shadows_enabled", &guiSceneSharedVars.shadows_enabled);
    studioBeginDisabled(!guiSceneSharedVars.shadows_enabled);
    if (beginStudioProperties("##shadows", propertyLabelWidth())) {
        studioPropertyLabel("Resolution", "Texels per side of each cascade's map");
        int resolution = guiSceneSharedVars.shadow_map_res_index;
        if (studioSegmented("##resolution",
              &resolution,
              std::span<const char *const>(guiSceneSharedVars.available_shadow_map_resolutions),
              {},
              -1.0F)) {
            guiSceneSharedVars.shadow_map_res_index = resolution;
            guiSceneSharedVars.shadow_resolution_changed = true;
        }
        studioPropertyLabel("Cascades", "Shadow maps along the view; more keep near shadows sharp");
        int cascades = guiSceneSharedVars.num_shadow_cascades - 1;
        const auto cascade_labels =
          std::span<const char *const>(kDigitLabels).subspan(1, static_cast<std::size_t>(MAX_CASCADES));
        if (studioSegmented("##cascades", &cascades, cascade_labels, {}, -1.0F)) {
            guiSceneSharedVars.num_shadow_cascades = cascades + 1;
            guiSceneSharedVars.shadow_resolution_changed = true;
        }
        studioPropertyLabel("PCF radius", "Filter taps on each side; 0 gives hard edges");
        studioSegmented("##pcf_radius",
          &guiSceneSharedVars.pcf_radius,
          std::span<const char *const>(kDigitLabels).first(static_cast<std::size_t>(MAX_PCF_RADIUS) + 1),
          {},
          -1.0F);
        studioPropertyLabel("Strength", "How dark shadowed areas get: 0 leaves them lit, 1 makes them black");
        studioSlider("##strength", &guiSceneSharedVars.cascaded_shadow_intensity, 0.0F, 1.0F, "%.2f");
        studioPropertyLabel("Distance", "How far from the camera the cascades reach");
        studioSlider("##distance",
          &guiSceneSharedVars.shadow_distance,
          kMinShadowDistance,
          kMaxShadowDistance,
          "%.0f",
          ImGuiSliderFlags_Logarithmic);
        studioPropertyLabel("Split lambda", "0 splits the cascades evenly, 1 logarithmically");
        studioSlider("##split_lambda", &guiSceneSharedVars.cascade_split_lambda, 0.0F, 1.0F, "%.2f");
        endStudioProperties();
    }
    studioEndDisabled();
}

void GUI::renderCameraInspector()
{
    if (beginStudioProperties("##lens", propertyLabelWidth())) {
        studioPropertyLabel("Field of view", "Vertical, in degrees");
        studioSlider("##fov", &guiSceneSharedVars.camera_fov, 20.0F, 110.0F, "%.0f\xC2\xB0");
        endStudioProperties();
    }

    sectionHeader("CONTROLS");
    renderCommonKeyBindings();
}

void GUI::renderModelInspector()
{
    const auto model_paths = sceneConfig::getAvailableModelPaths();
    const int loaded = guiSceneSharedVars.selected_model_index;
    const bool listed = loaded >= 0 && loaded < static_cast<int>(model_paths.size());
    // The scene holds a model the library may not list; the transform applies to it all the same.
    const bool has_model = !loadedModelName().empty();

    studioBeginDisabled(!has_model);
    if (beginStudioProperties("##transform", propertyLabelWidth())) {
        studioPropertyLabel("Position");
        if (studioDragAxes("##position", guiSceneSharedVars.model_position, 0.1F, 0.0F, 0.0F, "%.2f")) {
            guiSceneSharedVars.model_transform_changed = true;
        }
        studioPropertyLabel("Rotation", "Degrees about X, Y and Z");
        if (studioDragAxes("##rotation", guiSceneSharedVars.model_rotation, 0.5F, 0.0F, 0.0F, "%.1f\xC2\xB0")) {
            guiSceneSharedVars.model_transform_changed = true;
        }
        endStudioProperties();
    }
    if (ImGui::Button("Reset transform", ImVec2(-FLT_MIN, 0.0F))) {
        std::fill(std::begin(guiSceneSharedVars.model_position), std::end(guiSceneSharedVars.model_position), 0.0F);
        std::fill(std::begin(guiSceneSharedVars.model_rotation), std::end(guiSceneSharedVars.model_rotation), 0.0F);
        guiSceneSharedVars.model_transform_changed = true;
    }
    studioEndDisabled();

    sectionHeader("SOURCE");
    if (has_model) {
        ImGui::PushFont(studio->fonts.mono, 0.0F);
        const std::string &source =
          listed ? model_paths[static_cast<std::size_t>(loaded)] : guiRendererSharedVars.loadedModelPath;
        mutedWrappedText(source.c_str());
        ImGui::PopFont();
    } else {
        mutedWrappedText("Pick a model in the scene library.");
    }
}

void GUI::renderSkyInspector()
{
    sectionHeader("SKYBOX", "##skybox_enabled", &guiSceneSharedVars.skybox_enabled);

    sectionHeader("CLOUDS", "##clouds_enabled", &guiSceneSharedVars.clouds_enabled);
    studioBeginDisabled(!guiSceneSharedVars.clouds_enabled);
    if (beginStudioProperties("##clouds", propertyLabelWidth())) {
        studioPropertyLabel("March steps", "Samples along each view ray through the cloud box (cloud.num_march_steps)");
        studioSlider("##march_steps",
          &guiSceneSharedVars.cloud_num_march_steps,
          Kataglyphis::kMinCloudMarchSteps,
          Kataglyphis::kMaxCloudMarchSteps);
        studioPropertyLabel(
          "Light steps", "Samples from each view sample toward the sun (cloud.num_march_steps_to_light)");
        studioSlider("##light_steps",
          &guiSceneSharedVars.cloud_num_march_steps_to_light,
          Kataglyphis::kMinCloudLightMarchSteps,
          Kataglyphis::kMaxCloudLightMarchSteps);
        studioPropertyLabel("Density", "Multiplies every noise sample; the box keeps its size (cloud.scale)");
        studioSlider("##density",
          &guiSceneSharedVars.cloud_density_multiplier,
          Kataglyphis::kMinCloudDensityMultiplier,
          1.0F,
          "%.3f");
        studioPropertyLabel("Coverage", "Noise below this threshold is clear sky (cloud.threshold)");
        studioSlider("##coverage", &guiSceneSharedVars.cloud_coverage_threshold, 0.0F, 1.0F, "%.3f");
        studioPropertyLabel("Pillowness", "cloud.pillowness");
        studioSlider("##pillowness", &guiSceneSharedVars.cloud_pillowness, 0.0F, 1.0F, "%.3f");
        studioPropertyLabel("Cirrus", "cloud.cirrus_effect");
        studioSlider("##cirrus", &guiSceneSharedVars.cloud_cirrus_effect, 0.0F, 1.0F, "%.3f");
        studioPropertyLabel("Powder effect", "cloud.powder_effect");
        studioToggle("##powder_effect", &guiSceneSharedVars.cloud_powder_effect);
        // Logarithmic drags step in the normalized range, so 2.5 crosses 0.001..1000 in about 400 px.
        studioPropertyLabel("Scale", "Half extent of the cloud box in tens of units (cloud.radius)");
        studioDragAxes("##cloud_scale",
          guiSceneSharedVars.cloud_mesh_scale,
          2.5F,
          Kataglyphis::kMinCloudMeshExtent,
          1000.0F,
          "%.1f",
          ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
        studioPropertyLabel("Offset", "Centre of the cloud box (cloud.offset)");
        studioDragAxes("##cloud_offset",
          guiSceneSharedVars.cloud_mesh_offset,
          1.0F,
          -200.0F,
          400.0F,
          "%.1f",
          ImGuiSliderFlags_AlwaysClamp);
        endStudioProperties();
    }
    studioEndDisabled();
}

void GUI::renderRenderInspector()
{
    const int mode = displayedRenderMode();
    renderModeSwitch("##render_mode", -1.0F);
    if (!renderUserSelectionForRRT) {
        mutedWrappedText("Ray and path tracing need hardware ray tracing.");
    } else if (!guiRendererSharedVars.tracingReady) {
        mutedWrappedText(renderMode() != 0 ? "Your pick starts once a model has loaded; Raster cancels it."
                                           : "Ray and path tracing start once a model has loaded.");
    }

    studioBeginDisabled(mode != 0);
    if (beginStudioProperties("##raster", propertyLabelWidth())) {
        studioPropertyLabel("Raster path", "Forward shades while drawing; deferred shades a G-buffer afterwards");
        int raster_path = (guiRendererSharedVars.rasterizationMode == RasterizationMode::Deferred) ? 1 : 0;
        if (studioSegmented("##raster_path", &raster_path, kRasterPathLabels, {}, -1.0F)) {
            guiRendererSharedVars.rasterizationMode =
              (raster_path == 1) ? RasterizationMode::Deferred : RasterizationMode::Forward;
        }
        endStudioProperties();
    }
    studioEndDisabled();

    sectionHeader("PATH TRACING");
    studioBeginDisabled(mode != 2);
    if (beginStudioProperties("##path_tracing", propertyLabelWidth())) {
        studioPropertyLabel("Samples / pixel", "Paths per pixel and frame; a change restarts the accumulation");
        studioSlider("##samples_per_pixel", &guiRendererSharedVars.pathTracingSamplesPerPixel, 1, 64);
        studioPropertyLabel("Max bounces", "Segments per path; a change restarts the accumulation");
        studioSlider("##max_bounces", &guiRendererSharedVars.pathTracingMaxBounces, 1, 16);
        endStudioProperties();
    }
    studioEndDisabled();

    // Wrong culling has no visual signature beyond missing geometry; the switch tells whether it is the cause.
    sectionHeader("FRUSTUM CULLING", "##frustum_culling", &guiRendererSharedVars.frustum_culling_enabled);
    mutedWrappedText("Off draws every mesh and shadow caster. The profiler's Visibility tab counts both.");

    sectionHeader("SHADERS");
#ifndef NDEBUG
    if (ImGui::Button("Reload all shaders", ImVec2(-FLT_MIN, 0.0F))) {
        guiRendererSharedVars.shader_hot_reload_triggered = true;
    }
    ImGui::SetItemTooltip("F5");
#else
    studioBeginDisabled(true);
    ImGui::Button("Reload all shaders", ImVec2(-FLT_MIN, 0.0F));
    studioEndDisabled();
    ImGui::SetItemTooltip("Debug builds only");
#endif
}

void GUI::renderProfilerDock(float height)
{
    if (beginStudioDock("##studio_profiler", ImGuiDir_Down, height)) {
        if (ImGui::BeginTabBar("##profiler_tabs")) {
            if (studioTabItem("Performance")) {
                renderProfilerTab();
                ImGui::EndTabItem();
            }
            if (studioTabItem("Visibility")) {
                renderVisibilityTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    endStudioDock();
}

void GUI::renderProfilerTab()
{
    const ImGuiIO &io = ImGui::GetIO();
    const ImGuiStyle &style = ImGui::GetStyle();
    constexpr ImGuiTableFlags kFlags =
      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings;
    if (!ImGui::BeginTable("##performance", 2, kFlags)) { return; }
    ImGui::TableSetupColumn("frame", ImGuiTableColumnFlags_WidthStretch, 0.4F);
    ImGui::TableSetupColumn("gpu", ImGuiTableColumnFlags_WidthStretch, 0.6F);

    // Captions get their own row: a cell's baseline offset would otherwise carry into its neighbour.
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    studioCaption("FRAME");
    ImGui::TableSetColumnIndex(1);
    studioCaption("GPU PASSES");

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    const float frame_ms = (io.Framerate > 0.0F) ? 1000.0F / io.Framerate : 0.0F;
    const int decimals = (frame_ms >= 100.0F) ? 0 : ((frame_ms >= 10.0F) ? 1 : 2);
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%.*f", decimals, static_cast<double>(frame_ms));
    ImGui::PushFont(nullptr, kKataglyphisUiFontPixels * kStudioFigureScale);
    const float figure_ascent = ImGui::GetFontBaked()->Ascent;
    ImGui::TextUnformatted(text.data());
    ImGui::PopFont();
    ImGui::SameLine(0.0F, style.ItemInnerSpacing.x);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::round(figure_ascent - ImGui::GetFontBaked()->Ascent));
    mutedText("ms");
    std::snprintf(text.data(), text.size(), "%.0f FPS", static_cast<double>(io.Framerate));
    mutedText(text.data());

    if (studio->frameTimeCount > 0) {
        float peak = 1.0F;
        for (std::size_t sample = 0; sample < studio->frameTimeCount; ++sample) {
            peak = std::max(peak, studio->frameTimesMs[sample]);
        }
        const bool full = studio->frameTimeCount == studio->frameTimesMs.size();
        const float graph_height =
          std::max(ImGui::GetContentRegionAvail().y - style.CellPadding.y, ImGui::GetFrameHeight() * 1.5F);
        ImGui::PlotLines("##frame_times",
          studio->frameTimesMs.data(),
          static_cast<int>(studio->frameTimeCount),
          full ? static_cast<int>(studio->nextFrameTime) : 0,
          nullptr,
          0.0F,
          peak * 1.25F,
          ImVec2(-FLT_MIN, graph_height));
    }

    ImGui::TableSetColumnIndex(1);
    ImGui::PushStyleVarY(ImGuiStyleVar_CellPadding, std::round(style.CellPadding.y * 0.5F));
    renderGpuPassTimings(guiRendererSharedVars.gpuTimings.supported,
      guiRendererSharedVars.gpuTimings.pass_ms,
      GPU_TIMED_PASS_NAMES,
      frame_ms);
    ImGui::PopStyleVar();

    ImGui::EndTable();
}

void GUI::renderVisibilityTab()
{
    const auto &visibility = guiRendererSharedVars.visibility;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Frustum culling");
    ImGui::SameLine();
    studioToggle("##visibility_culling", &guiRendererSharedVars.frustum_culling_enabled);

    ImGui::PushStyleVarY(ImGuiStyleVar_CellPadding, std::round(ImGui::GetStyle().CellPadding.y * 0.5F));
    if (ImGui::BeginTable("##visibility", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("bar", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("count", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("00000 / 00000").x);
        countRow("Meshes drawn", visibility.meshes_drawn, visibility.meshes_total);
        countRow("Shadow casters drawn", visibility.shadow_casters_drawn, visibility.shadow_casters_total);
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::PushStyleColor(ImGuiCol_Text, studio_palette::kTextMuted);
    ImGui::Text("Culled: %u meshes (%.1f %%), %u shadow casters (%.1f %%)",
      visibility.meshes_total - visibility.meshes_drawn,
      culledPercent(visibility.meshes_drawn, visibility.meshes_total),
      visibility.shadow_casters_total - visibility.shadow_casters_drawn,
      culledPercent(visibility.shadow_casters_drawn, visibility.shadow_casters_total));
    ImGui::PopStyleColor();
}

void GUI::renderViewportOverlay(float sceneX0, float sceneY0, float sceneX1, float sceneY1)
{
    const ImGuiStyle &style = ImGui::GetStyle();
    const ImGuiIO &io = ImGui::GetIO();
    const int mode = displayedRenderMode();

    std::array<char, 96> label{};
    if (mode == 2) {
        std::snprintf(label.data(),
          label.size(),
          "Path traced  \xC2\xB7  %d spp  \xC2\xB7  %d bounces",
          guiRendererSharedVars.pathTracingSamplesPerPixel,
          guiRendererSharedVars.pathTracingMaxBounces);
    } else if (mode == 1) {
        std::snprintf(label.data(), label.size(), "Ray traced");
    } else {
        std::snprintf(label.data(),
          label.size(),
          "Raster  \xC2\xB7  %s",
          (guiRendererSharedVars.rasterizationMode == RasterizationMode::Deferred) ? "Deferred" : "Forward");
    }
    std::array<char, 32> extent{};
    std::snprintf(extent.data(),
      extent.size(),
      "%d \xC3\x97 %d",
      static_cast<int>((sceneX1 - sceneX0) * io.DisplayFramebufferScale.x),
      static_cast<int>((sceneY1 - sceneY0) * io.DisplayFramebufferScale.y));

    const ImVec2 label_size = ImGui::CalcTextSize(label.data());
    const float extent_width = ImGui::CalcTextSize(extent.data()).x;
    const float dot = ImGui::GetFontSize() * 0.5F;
    const float chip_height = ImGui::GetFrameHeight();
    const float chip_width = style.FramePadding.x * 2.0F + dot + style.ItemInnerSpacing.x + label_size.x
                             + style.ItemSpacing.x * 1.5F + extent_width;
    // The same top band as the dock headers: the vertical window padding, not the horizontal one.
    const ImVec2 chip_max(sceneX1 - style.WindowPadding.x, sceneY0 + style.WindowPadding.y + chip_height);
    const ImVec2 chip_min(chip_max.x - chip_width, sceneY0 + style.WindowPadding.y);
    if (chip_min.x < sceneX0 + style.WindowPadding.x || chip_max.y > sceneY1 - style.WindowPadding.y) { return; }

    // The background list draws above the scene and below every window, and takes no input.
    ImDrawList *drawList = ImGui::GetBackgroundDrawList();
    const float rounding = chip_height * 0.5F;
    drawList->AddRectFilled(
      chip_min, chip_max, ImGui::GetColorU32(studioWithAlpha(studio_palette::kPanel, 0.86F)), rounding);
    drawList->AddRect(chip_min, chip_max, ImGui::GetColorU32(studio_palette::kBorder), rounding);
    float x = chip_min.x + style.FramePadding.x;
    const float text_y = chip_min.y + (chip_height - label_size.y) * 0.5F;
    drawList->AddCircleFilled(ImVec2(x + dot * 0.5F, chip_min.y + chip_height * 0.5F),
      dot * 0.36F,
      ImGui::GetColorU32(studio_palette::kAccent));
    x += dot + style.ItemInnerSpacing.x;
    drawList->AddText(ImVec2(x, text_y), ImGui::GetColorU32(studio_palette::kText), label.data());
    x += label_size.x + style.ItemSpacing.x * 1.5F;
    drawList->AddText(ImVec2(x, text_y), ImGui::GetColorU32(studio_palette::kTextMuted), extent.data());
}

void GUI::renderFloatingWindows()
{
    const float font_size = ImGui::GetFontSize();
    if (studio->showStyleEditor) {
        ImGui::SetNextWindowSize(ImVec2(font_size * 30.0F, font_size * 40.0F), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Style editor", &studio->showStyleEditor)) { ImGui::ShowStyleEditor(); }
        ImGui::End();
    }
    if (studio->showMetrics) { ImGui::ShowMetricsWindow(&studio->showMetrics); }

    if (studio->openAbout) {
        ImGui::OpenPopup("About##studio_about");
        studio->openAbout = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
    if (ImGui::BeginPopupModal(
          "About##studio_about", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        const float mark = font_size * 2.4F;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        drawStudioIcon(
          ImGui::GetWindowDrawList(), StudioIcon::Cube, origin, mark, ImGui::GetColorU32(studio_palette::kAccent));
        ImGui::Dummy(ImVec2(mark, mark));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushFont(nullptr, kKataglyphisUiFontPixels * kStudioTitleScale);
        ImGui::TextUnformatted("BeschleunigerBallett");
        ImGui::PopFont();
        mutedText("A Vulkan graphics engine playground");
        ImGui::EndGroup();
        ImGui::Spacing();

        std::array<char, 32> api{};
        formatApiVersion(api, studio->apiVersion);
        const float value_x = font_size * 6.5F;
        const auto about_row = [value_x](const char *name, const char *value) {
            mutedText(name);
            ImGui::SameLine(value_x);
            ImGui::TextUnformatted(value);
        };
        about_row("Version", PROJECT_VERSION);
        about_row("Build", kBuildType);
        about_row("GPU", studio->deviceName.c_str());
        about_row("API", api.data());
        about_row("Dear ImGui", IMGUI_VERSION);
        ImGui::Spacing();
        ImGui::TextLinkOpenURL("github.com/Kataglyphis/BeschleunigerBallett", kRepositoryUrl);
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(font_size * 8.0F, 0.0F)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void GUI::cleanUp()
{
    if (device) {
        ImGui_ImplVulkan_Shutdown();
        device->getLogicalDevice().destroyDescriptorPool(gui_descriptor_pool);
        gui_descriptor_pool = nullptr;
        device.reset();
    }
    // Runs even without a device, or a device-lost shutdown leaks the ImGui context; the guard keeps it idempotent.
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
}

void GUI::create_gui_context(Window *frontend_window,
  const vk::Instance &instance,
  vk::Format post_color_format,
  uint32_t image_count)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();

    // The layout is code; an imgui.ini would only pin the floating tool windows into the working directory.
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // A focused panel would otherwise hold WASD until the scene is clicked; Esc leaves it (render()).
    io.ConfigNavCaptureKeyboard = false;

    studio->fonts = Kataglyphis::Frontend::configureKataglyphisImGuiFonts(io);
    // render() rescales the theme on its first frame, once the window's content scale is known.
    Kataglyphis::Frontend::applyKataglyphisImGuiDarkTheme();

    const vk::PhysicalDeviceProperties properties = device->getPhysicalDevice().getProperties();
    studio->deviceName = std::string(properties.deviceName.data());
    studio->apiVersion = properties.apiVersion;
    studio->cpuDevice = properties.deviceType == vk::PhysicalDeviceType::eCpu;

    ImGui_ImplGlfw_InitForVulkan(frontend_window->get_window(), false);

    // Create Descriptor Pool
    std::array<vk::DescriptorPoolSize, 11> gui_pool_sizes = { { { vk::DescriptorType::eSampler, 10 },
      { vk::DescriptorType::eCombinedImageSampler, 10 },
      { vk::DescriptorType::eSampledImage, 10 },
      { vk::DescriptorType::eStorageImage, 10 },
      { vk::DescriptorType::eUniformTexelBuffer, 10 },
      { vk::DescriptorType::eStorageTexelBuffer, 10 },
      { vk::DescriptorType::eUniformBuffer, 10 },
      { vk::DescriptorType::eStorageBuffer, 10 },
      { vk::DescriptorType::eUniformBufferDynamic, 10 },
      { vk::DescriptorType::eStorageBufferDynamic, 10 },
      { vk::DescriptorType::eInputAttachment, 100 } } };

    vk::DescriptorPoolCreateInfo gui_pool_info{};
    gui_pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    gui_pool_info.maxSets = 10 * static_cast<uint32_t>(gui_pool_sizes.size());
    gui_pool_info.poolSizeCount = static_cast<uint32_t>(gui_pool_sizes.size());
    gui_pool_info.pPoolSizes = gui_pool_sizes.data();

    auto pool_result = device->getLogicalDevice().createDescriptorPool(gui_pool_info);
    ASSERT_VULKAN(pool_result.result, "Failed to create a gui descriptor pool!");
    gui_descriptor_pool = pool_result.value;

    Kataglyphis::VulkanRendererInternals::QueueFamilyIndices const indices = device->getQueueFamilies();

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = instance;
    init_info.PhysicalDevice = device->getPhysicalDevice();
    init_info.Device = device->getLogicalDevice();
    init_info.QueueFamily = static_cast<uint32_t>(indices.graphics_family);
    init_info.Queue = device->getGraphicsQueue();
    // Core 1.3 vkCmdBeginRendering, not the KHR alias: the device requires Vulkan 1.3.
    init_info.ApiVersion = VK_API_VERSION_1_3;
    // Drawn inside the post pass's vkCmdBeginRendering; imgui deep-copies the format array.
    init_info.UseDynamicRendering = true;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo = {};
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    const VkFormat color_format = static_cast<VkFormat>(post_color_format);
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format;
    init_info.DescriptorPool = gui_descriptor_pool;
    init_info.PipelineCache = device->getPipelineCache();
    init_info.MinImageCount = 2;
    init_info.ImageCount = image_count;
    init_info.Allocator = VK_NULL_HANDLE;
    init_info.CheckVkResultFn = VK_NULL_HANDLE;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    ImGui_ImplVulkan_Init(&init_info);
}

GUI::~GUI() { cleanUp(); }
