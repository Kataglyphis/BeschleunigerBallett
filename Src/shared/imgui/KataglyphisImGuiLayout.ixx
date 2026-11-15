module;
#include <algorithm>
#include <cmath>

#include <imgui.h>
#include <imgui_internal.h>

export module kataglyphis.shared.imgui.layout;

export namespace Kataglyphis::Frontend {

// NoFocusOnAppearing: a focused dock would hold the keyboard and swallow the camera's WASD.
inline constexpr ImGuiWindowFlags kStudioDockFlags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse
                                                     | ImGuiWindowFlags_NoBringToFrontOnFocus
                                                     | ImGuiWindowFlags_NoFocusOnAppearing;

// One pixel on the scene side of the current bar window, in place of a border on all four sides.
inline void studioBarHairline(ImGuiDir dir)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    const ImRect rect = window->Rect();
    ImRect line = rect;
    switch (dir) {
    case ImGuiDir_Left:
        line.Min.x = rect.Max.x - 1.0F;
        break;
    case ImGuiDir_Right:
        line.Max.x = rect.Min.x + 1.0F;
        break;
    case ImGuiDir_Up:
        line.Min.y = rect.Max.y - 1.0F;
        break;
    default:
        line.Max.y = rect.Min.y + 1.0F;
        break;
    }
    window->DrawList->PushClipRect(rect.Min, rect.Max, false);
    window->DrawList->AddRectFilled(line.Min, line.Max, ImGui::GetColorU32(ImGuiCol_Border));
    window->DrawList->PopClipRect();
}

// Like Begin(), the matching endStudioDock() runs whatever this returns.
inline auto beginStudioDock(const char *name, ImGuiDir dir, float size, ImGuiWindowFlags extraFlags = 0) -> bool
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    const bool open =
      ImGui::BeginViewportSideBar(name, ImGui::GetMainViewport(), dir, size, kStudioDockFlags | extraFlags);
    ImGui::PopStyleVar();
    studioBarHairline(dir);
    return open;
}

inline void endStudioDock() { ImGui::End(); }

// What no bar has claimed this frame: the part of the swapchain image the scene shows through.
inline auto studioSceneRect() -> ImRect
{ return static_cast<ImGuiViewportP *>(ImGui::GetMainViewport())->GetBuildWorkRect(); }

// Where in the handle the press landed; like ImGui's own splitters, the drag re-anchors on the handle every frame.
struct StudioSplitterDrag
{
    bool active = false;
    float grabOffset = 0.0F;
};

// Inside the scene's edge, free of windows; `growth` is +1 if dragging along the axis grows the dock, -1 if it shrinks.
inline auto studioDockSplitter(ImRect handle,
  ImGuiAxis axis,
  float growth,
  float *size,
  float minSize,
  float maxSize,
  StudioSplitterDrag *drag) -> bool
{
    const ImGuiContext &context = *ImGui::GetCurrentContext();
    const ImGuiIO &io = ImGui::GetIO();
    const float mouse = (axis == ImGuiAxis_X) ? io.MousePos.x : io.MousePos.y;
    const float edge = (axis == ImGuiAxis_X) ? handle.Min.x : handle.Min.y;
    bool resized = false;
    // The release frame's position counts, being reached with the button held; off the window the position is invalid.
    if (drag->active && ImGui::IsMousePosValid()) {
        // From where the handle is now, so its edge stays under the cursor past a stop and through window resizes.
        const float target = *size + (mouse - drag->grabOffset - edge) * growth;
        // Whole pixels, so the rounded dock extent can reach a .5 stop.
        const float next = std::round(std::clamp(target, minSize, std::max(minSize, maxSize)));
        resized = next != *size;
        *size = next;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { drag->active = false; }
    // Only a free mouse hovers: a press held from the scene, another splitter or look mode must not light or grab one.
    const bool mouseFree = (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                           && !ImGui::IsMouseDown(ImGuiMouseButton_Right)
                           && !ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    // WantCaptureMouse is set while a popup or modal is open, which must block the handle like any item.
    const bool hovered = !drag->active && mouseFree && !io.WantCaptureMouse && context.HoveredWindow == nullptr
                         && ImGui::IsMouseHoveringRect(handle.Min, handle.Max, false);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        *drag = StudioSplitterDrag{ .active = true, .grabOffset = mouse - edge };
    }
    if (hovered || drag->active) {
        ImGui::SetMouseCursor(axis == ImGuiAxis_X ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
        // Background list: above the scene but below any floating window that covers the handle.
        ImGui::GetBackgroundDrawList()->AddRectFilled(handle.Min,
          handle.Max,
          ImGui::GetColorU32(drag->active ? ImGuiCol_SeparatorActive : ImGuiCol_SeparatorHovered));
    }
    // The caller stores only a real resize, so a click or a held handle never rewrites a dock's preference.
    return resized;
}
}// namespace Kataglyphis::Frontend
