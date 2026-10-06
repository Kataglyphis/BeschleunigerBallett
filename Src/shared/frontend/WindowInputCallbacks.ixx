module;

#include <GLFW/glfw3.h>
#include <imgui.h>

#include "WindowInputState.hpp"

export module kataglyphis.shared.frontend.window_input_callbacks;

export namespace Kataglyphis::Frontend {

using Kataglyphis::Frontend::window_key_count;

inline void reset_window_keys(bool *keys)
{
    for (std::size_t index = 0; index < window_key_count; ++index) { keys[index] = false; }
}

inline void handle_focus_lost(bool *keys, bool &mouse_first_moved, bool &look_mode_active)
{
    reset_window_keys(keys);
    mouse_first_moved = true;
    look_mode_active = false;
}

// Producers (handle_mouse_callback) accumulate into axis_change so several
// cursor events landing in one frame - a 1000 Hz mouse at 60 FPS delivers
// ~16 - are not lost; the frame loop consumes exactly once per frame via
// this function, which reads the total and resets it for the next frame.
inline float consume_axis_delta(float &axis_change)
{
    float const delta = axis_change;
    axis_change = 0.0F;
    return delta;
}

inline bool imgui_wants_keyboard_capture()
{ return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureKeyboard; }

inline void handle_key_callback(GLFWwindow *window, bool *keys, int key, int action)
{
    bool const captured = imgui_wants_keyboard_capture();

    // RELEASE must always fall through, even while ImGui holds capture -
    // otherwise a key held when a widget grabs focus is never released and
    // the camera keeps moving forever.
    if (action == GLFW_RELEASE) {
        if (key >= 0 && static_cast<std::size_t>(key) < window_key_count) { keys[key] = false; }
        return;
    }

    if (captured) { return; }

    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) { glfwSetWindowShouldClose(window, GLFW_TRUE); }

    if (key >= 0 && static_cast<std::size_t>(key) < window_key_count) {
        if (action == GLFW_PRESS) { keys[key] = true; }
    }
}

inline void handle_mouse_callback(GLFWwindow *window,
  float &last_x,
  float &last_y,
  float &x_change,
  float &y_change,
  bool &mouse_first_moved,
  bool look_mode_active,
  double x_pos,
  double y_pos)
{
    (void)window;
    if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse) {
        // Keep tracking the raw cursor position while ImGui holds capture,
        // even though no delta is emitted - otherwise last_x/last_y go stale
        // at the pre-panel position and the first event after capture ends
        // differences against it, snapping the camera by the distance
        // crossed while hovering the panel.
        last_x = static_cast<float>(x_pos);
        last_y = static_cast<float>(y_pos);
        return;
    }

    if (!look_mode_active) {
        // The cursor-pos callback is installed unconditionally (ImGui backend
        // requirement), so events keep arriving outside look mode too. Track
        // position without emitting a delta, for the same reason as the
        // ImGui-capture branch above: otherwise the first event after look
        // mode resumes differences against a stale position and snaps.
        last_x = static_cast<float>(x_pos);
        last_y = static_cast<float>(y_pos);
        return;
    }

    // Re-seeding first, then accumulating below, makes the "first event after
    // (re)capture produces no delta" property hold by construction: the diff
    // against the just-seeded last position is exactly zero, so it adds
    // nothing to whatever x_change/y_change already hold.
    if (mouse_first_moved) {
        last_x = static_cast<float>(x_pos);
        last_y = static_cast<float>(y_pos);
        mouse_first_moved = false;
    }

    x_change += static_cast<float>(x_pos) - last_x;
    y_change += last_y - static_cast<float>(y_pos);

    last_x = static_cast<float>(x_pos);
    last_y = static_cast<float>(y_pos);
}

inline bool imgui_wants_mouse_capture()
{ return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse; }

inline int cursor_input_mode_for(bool look_mode_active)
{ return look_mode_active ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL; }

inline bool should_capture_cursor(int button, int action)
{ return (action == GLFW_PRESS) && (button == GLFW_MOUSE_BUTTON_RIGHT); }

// Start and stop cannot share one predicate: "not a right-press" also
// matches unrelated buttons (left click, middle click, ...), so treating
// its negation as "release look mode" ends look mode on input that never
// started it.
inline bool should_release_cursor(int button, int action)
{ return (action == GLFW_RELEASE) && (button == GLFW_MOUSE_BUTTON_RIGHT); }

inline void handle_mouse_button_callback(GLFWwindow *window,
  bool &mouse_first_moved,
  bool &look_mode_active,
  int button,
  int action)
{
    (void)window;
    if (should_capture_cursor(button, action)) {
        if (imgui_wants_mouse_capture()) { return; }
        // Entering look mode must always re-seed, not just leaving it -
        // otherwise the first drag of every run snaps the camera by the
        // cursor's absolute screen position.
        mouse_first_moved = true;
        look_mode_active = true;
    } else if (should_release_cursor(button, action)) {
        // A release must always end look mode, even while ImGui holds
        // capture - otherwise releasing the mouse over a panel keeps
        // steering the camera forever.
        mouse_first_moved = true;
        look_mode_active = false;
    }
}

}// namespace Kataglyphis::Frontend
