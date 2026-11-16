#pragma once

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Kataglyphis::TestSupport {

/// Sends input through the window's own GLFW callbacks, one press or release per frame as ImGui takes them.
class ImGuiInputDriver
{
  public:
    ImGuiInputDriver(GLFWwindow *window, std::function<void()> renderOneFrame)
      : window_(window), renderOneFrame_(std::move(renderOneFrame))
    {
        // GLFW has no getter: setting a callback returns the one it replaces, which goes straight back.
        cursorEnter_ = glfwSetCursorEnterCallback(window_, nullptr);
        glfwSetCursorEnterCallback(window_, cursorEnter_);
        cursorPos_ = glfwSetCursorPosCallback(window_, nullptr);
        glfwSetCursorPosCallback(window_, cursorPos_);
        mouseButton_ = glfwSetMouseButtonCallback(window_, nullptr);
        glfwSetMouseButtonCallback(window_, mouseButton_);
        key_ = glfwSetKeyCallback(window_, nullptr);
        glfwSetKeyCallback(window_, key_);
        // A pointer outside the window makes ImGui's backend poll the OS cursor, which would undo every move here.
        if (cursorEnter_ != nullptr) { cursorEnter_(window_, GLFW_TRUE); }
    }

    /// False if the window installed no input callbacks, so nothing sent here could reach ImGui.
    [[nodiscard]] bool wired() const
    { return cursorEnter_ != nullptr && cursorPos_ != nullptr && mouseButton_ != nullptr && key_ != nullptr; }

    void frames(int count)
    {
        for (int frame = 0; frame < count; ++frame) { renderOneFrame_(); }
    }

    void moveTo(ImVec2 point)
    {
        cursorPos_(window_, static_cast<double>(point.x), static_cast<double>(point.y));
        frames(1);
    }

    // The hover frame first: a row that allows overlap only takes a press it was hovered for the frame before.
    void click(ImVec2 point)
    {
        moveTo(point);
        button(GLFW_PRESS);
        button(GLFW_RELEASE);
    }

    void doubleClick(ImVec2 point)
    {
        moveTo(point);
        for (int press = 0; press < 2; ++press) {
            button(GLFW_PRESS);
            button(GLFW_RELEASE);
        }
    }

    void pressKey(int glfwKey)
    {
        sendKey(glfwKey, GLFW_PRESS);
        sendKey(glfwKey, GLFW_RELEASE);
    }

    /// The first point on `from`..`to`, `step` px apart, where ImGui's hit test reports `id` hovered.
    std::optional<ImVec2> pointAt(ImGuiID id, ImVec2 from, ImVec2 to, float step)
    {
        const float length = std::hypot(to.x - from.x, to.y - from.y);
        const int samples = 1 + static_cast<int>(length / step);
        for (int sample = 0; sample <= samples; ++sample) {
            const float t = (samples > 0) ? static_cast<float>(sample) / static_cast<float>(samples) : 0.0F;
            const ImVec2 point(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t);
            moveTo(point);
            if (ImGui::GetCurrentContext()->HoveredId == id) { return point; }
        }
        return std::nullopt;
    }

    /// pointAt down `area` at `x`; half a frame height apart, no row can fall between two points.
    std::optional<ImVec2> pointAtOnVertical(ImGuiID id, const ImRect &area, float x)
    {
        return pointAt(id,
          ImVec2(x, area.Min.y + 1.0F),
          ImVec2(x, area.Max.y - 1.0F),
          std::max(1.0F, ImGui::GetFrameHeight() * 0.5F));
    }

  private:
    void button(int action)
    {
        mouseButton_(window_, GLFW_MOUSE_BUTTON_LEFT, action, 0);
        frames(1);
    }

    void sendKey(int glfwKey, int action)
    {
        key_(window_, glfwKey, glfwGetKeyScancode(glfwKey), action, 0);
        frames(1);
    }

    GLFWwindow *window_;
    std::function<void()> renderOneFrame_;
    GLFWcursorenterfun cursorEnter_ = nullptr;
    GLFWcursorposfun cursorPos_ = nullptr;
    GLFWmousebuttonfun mouseButton_ = nullptr;
    GLFWkeyfun key_ = nullptr;
};

/// The child `childStrId` of `parentName` drawn last frame; a tab's child exists per tab, only the shown one active.
inline ImGuiWindow *activeChildWindow(const char *parentName, const char *childStrId)
{
    ImGuiWindow *parent = ImGui::FindWindowByName(parentName);
    if (parent == nullptr) { return nullptr; }
    // BeginChild names a child "<parent>/<str_id>_<id>".
    const std::string prefix = std::string(parent->Name) + "/" + childStrId + "_";
    for (ImGuiWindow *window : ImGui::GetCurrentContext()->Windows) {
        if (window->Active && window->ParentWindow == parent && std::string_view(window->Name).starts_with(prefix)) {
            return window;
        }
    }
    return nullptr;
}

}// namespace Kataglyphis::TestSupport
