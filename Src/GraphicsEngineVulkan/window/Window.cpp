module;
#include "GLFW/glfw3.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

#include <vulkan/vulkan.hpp>

module kataglyphis.vulkan.window;

import kataglyphis.shared.frontend.window_input_callbacks;

using namespace Kataglyphis::Frontend;
// GLFW Callback functions
static void onErrorCallback(int error, const char *description)
{ std::cerr << "GLFW Error " << error << ": " << description << '\n'; }

// Win32 and X11 size windows in pixels, so the size follows the scale; Cocoa and Wayland use logical units.
static void fitToPrimaryMonitor(uint32_t &window_width, uint32_t &window_height)
{
    GLFWmonitor *monitor = glfwGetPrimaryMonitor();
    if (monitor == nullptr) { return; }

    float scale_x = 1.0F;
    float scale_y = 1.0F;
    glfwGetMonitorContentScale(monitor, &scale_x, &scale_y);
    scale_x = std::max(scale_x, 1.0F);
    scale_y = std::max(scale_y, 1.0F);
    int area_x = 0;
    int area_y = 0;
    int area_width = 0;
    int area_height = 0;
    glfwGetMonitorWorkarea(monitor, &area_x, &area_y, &area_width, &area_height);
    auto area_w = static_cast<float>(area_width);
    auto area_h = static_cast<float>(area_height);

    const int platform = glfwGetPlatform();
    const bool pixel_sized = platform == GLFW_PLATFORM_WIN32 || platform == GLFW_PLATFORM_X11;
    // Wayland: pixel work area, logical windows; a fractional scale reads as its integer ceiling, so this errs small.
    if (platform == GLFW_PLATFORM_WAYLAND) {
        area_w /= scale_x;
        area_h /= scale_y;
    }

    const auto fit = [](uint32_t size, float scale, float area) -> uint32_t {
        const float scaled = static_cast<float>(size) * scale;
        const float limit = (area > 0.0F) ? area * 0.9F : scaled;
        return static_cast<uint32_t>(std::min(scaled, limit));
    };
    window_width = fit(window_width, pixel_sized ? scale_x : 1.0F, area_w);
    window_height = fit(window_height, pixel_sized ? scale_y : 1.0F, area_h);
}

Window::Window() : framebuffer_resized(false)
{
    Kataglyphis::Frontend::reset_window_keys(input_state.keys.data());

    if (initialize(800, 600) != 0) { spdlog::critical("Window initialization failed!"); }
}

// please use this constructor; never the standard
Window::Window(uint32_t window_width, uint32_t window_height, bool fit_to_monitor) : framebuffer_resized(false)
{
    Kataglyphis::Frontend::reset_window_keys(input_state.keys.data());

    if (initialize(window_width, window_height, fit_to_monitor) != 0) {
        spdlog::critical("Window initialization failed!");
    }
}

auto Window::initialize(uint32_t window_width, uint32_t window_height, bool fit_to_monitor) -> int
{
    glfwSetErrorCallback(onErrorCallback);
    if (glfwInit() == 0) {
        std::cerr << "GLFW Init failed!" << '\n';
        glfwTerminate();
        return 1;
    }

    if (glfwVulkanSupported() == 0) { spdlog::error("No Vulkan Supported!"); }

    if (fit_to_monitor) { fitToPrimaryMonitor(window_width, window_height); }

    // allow it to resize
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    // retrieve new window
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    main_window = glfwCreateWindow(
      static_cast<int>(window_width), static_cast<int>(window_height), "BeschleunigerBallett", nullptr, nullptr);

    if (main_window == nullptr) {
        std::cerr << "GLFW Window creation failed!" << '\n';
        glfwTerminate();
        return 1;
    }

    init_callbacks();

    initialized = true;
    return 0;
}

void Window::cleanUp()
{
    if (main_window == nullptr) { return; }
    glfwDestroyWindow(main_window);
    main_window = nullptr;
    glfwTerminate();
}

auto Window::get_x_change() -> float { return Kataglyphis::Frontend::consume_axis_delta(input_state.x_change); }

auto Window::get_y_change() -> float { return Kataglyphis::Frontend::consume_axis_delta(input_state.y_change); }

auto Window::framebuffer_size_has_changed() const -> bool { return framebuffer_resized; }

void Window::init_callbacks()
{
    glfwSetWindowUserPointer(main_window, this);
    glfwSetKeyCallback(main_window, &key_callback);
    glfwSetMouseButtonCallback(main_window, &mouse_button_callback);
    glfwSetScrollCallback(main_window, &scroll_callback);
    glfwSetCharCallback(main_window, &char_callback);
    glfwSetFramebufferSizeCallback(main_window, &framebuffer_size_callback);
    glfwSetWindowFocusCallback(main_window, &window_focus_callback);
    glfwSetCursorEnterCallback(main_window, &cursor_enter_callback);
    glfwSetDropCallback(main_window, &drop_callback);
    // Always installed: ImGui's GLFW backend has no callbacks of its own and needs it; look mode gates the camera.
    glfwSetCursorPosCallback(main_window, &mouse_callback);
}

void Window::framebuffer_size_callback(GLFWwindow *window, int width, int height)
{
    // Only flags the change; the swapchain rebuild queries the size itself.
    (void)width;
    (void)height;
    auto *app = reinterpret_cast<Window *>(glfwGetWindowUserPointer(window));
    app->framebuffer_resized = true;
}

void Window::window_focus_callback(GLFWwindow *window, int focused)
{
    ImGui_ImplGlfw_WindowFocusCallback(window, focused);

    if (focused == GLFW_FALSE) {
        auto *the_window = static_cast<Window *>(glfwGetWindowUserPointer(window));
        Kataglyphis::Frontend::handle_focus_lost(the_window->input_state.keys.data(),
          the_window->input_state.mouse_first_moved,
          the_window->input_state.look_mode_active);

        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        if (glfwRawMouseMotionSupported()) { glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE); }
    }
}

void Window::cursor_enter_callback(GLFWwindow *window, int entered)
{ ImGui_ImplGlfw_CursorEnterCallback(window, entered); }

void Window::reset_framebuffer_has_changed() { this->framebuffer_resized = false; }

auto Window::take_dropped_paths() -> std::vector<std::filesystem::path> { return std::exchange(dropped_paths, {}); }

void Window::drop_callback(GLFWwindow *window, int count, const char **paths)
{
    auto *the_window = static_cast<Window *>(glfwGetWindowUserPointer(window));
    for (int index = 0; index < count; ++index) {
        // GLFW hands over UTF-8, which a narrow path would read in the Windows code page.
        const std::string_view utf8(paths[index]);
        the_window->dropped_paths.emplace_back(std::u8string(utf8.begin(), utf8.end()));
    }
}

void Window::key_callback(GLFWwindow *window, int key, int scancode, int action, int mods)
{
    ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);

    auto *the_window = static_cast<Window *>(glfwGetWindowUserPointer(window));
    Kataglyphis::Frontend::handle_key_callback(window, the_window->input_state.keys.data(), key, action);
}

void Window::mouse_callback(GLFWwindow *window, double x_pos, double y_pos)
{
    ImGui_ImplGlfw_CursorPosCallback(window, x_pos, y_pos);

    auto *the_window = static_cast<Window *>(glfwGetWindowUserPointer(window));
    Kataglyphis::Frontend::handle_mouse_callback(window,
      the_window->input_state.last_x,
      the_window->input_state.last_y,
      the_window->input_state.x_change,
      the_window->input_state.y_change,
      the_window->input_state.mouse_first_moved,
      the_window->input_state.look_mode_active,
      x_pos,
      y_pos);
}

void Window::mouse_button_callback(GLFWwindow *window, int button, int action, int mods)
{
    ImGui_ImplGlfw_MouseButtonCallback(window, button, action, mods);

    auto *the_window = static_cast<Window *>(glfwGetWindowUserPointer(window));
    bool const was_look_mode_active = the_window->input_state.look_mode_active;
    Kataglyphis::Frontend::handle_mouse_button_callback(
      window, the_window->input_state.mouse_first_moved, the_window->input_state.look_mode_active, button, action);

    bool const is_look_mode_active = the_window->input_state.look_mode_active;
    if (is_look_mode_active != was_look_mode_active) {
        glfwSetInputMode(window, GLFW_CURSOR, Kataglyphis::Frontend::cursor_input_mode_for(is_look_mode_active));
        if (glfwRawMouseMotionSupported()) {
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, is_look_mode_active ? GLFW_TRUE : GLFW_FALSE);
        }
        // X11 and Wayland report no motion for GLFW's warp back, so ImGui would keep look mode's virtual position.
        if (!is_look_mode_active) {
            double x_pos = 0.0;
            double y_pos = 0.0;
            glfwGetCursorPos(window, &x_pos, &y_pos);
            ImGui_ImplGlfw_CursorPosCallback(window, x_pos, y_pos);
        }
    }
}

void Window::scroll_callback(GLFWwindow *window, double x_offset, double y_offset)
{ ImGui_ImplGlfw_ScrollCallback(window, x_offset, y_offset); }

void Window::char_callback(GLFWwindow *window, unsigned int codepoint)
{ ImGui_ImplGlfw_CharCallback(window, codepoint); }

Window::~Window() { cleanUp(); }
