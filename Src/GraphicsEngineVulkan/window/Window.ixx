module;

#include <GLFW/glfw3.h>

#include "../../shared/frontend/WindowInputState.hpp"

#include <filesystem>
#include <span>
#include <vector>

export module kataglyphis.vulkan.window;

export namespace Kataglyphis::Frontend {
class Window
{
  public:
    Window();
    // `fit_to_monitor` scales the size by the monitor's content scale, within 90 % of its work area; the tests
    // leave it off, since their goldens need exact framebuffer sizes.
    Window(uint32_t window_width, uint32_t window_height, bool fit_to_monitor = false);

    [[nodiscard]] int initialize(uint32_t window_width, uint32_t window_height, bool fit_to_monitor = false);
    void cleanUp();

    bool get_should_close() const { return glfwWindowShouldClose(main_window); }
    float get_x_change();
    float get_y_change();
    GLFWwindow *get_window() const { return main_window; }

    std::span<const bool> get_keys() const { return input_state.keys; }
    bool framebuffer_size_has_changed() const;
    void reset_framebuffer_has_changed();
    [[nodiscard]] bool is_valid() const { return initialized && main_window != nullptr; }
    // The files dropped on the window since the last call, in the order the platform gave them.
    [[nodiscard]] std::vector<std::filesystem::path> take_dropped_paths();

    ~Window();

  private:
    GLFWwindow *main_window{};
    Kataglyphis::Frontend::WindowInputState input_state;
    bool framebuffer_resized;
    bool initialized{ false };
    std::vector<std::filesystem::path> dropped_paths;

    void init_callbacks();
    static void framebuffer_size_callback(GLFWwindow *window, int width, int height);
    static void key_callback(GLFWwindow *window, int key, int scancode, int action, int mods);
    static void mouse_callback(GLFWwindow *window, double x_pos, double y_pos);
    static void mouse_button_callback(GLFWwindow *window, int button, int action, int mods);
    static void scroll_callback(GLFWwindow *window, double x_offset, double y_offset);
    static void char_callback(GLFWwindow *window, unsigned int codepoint);
    static void window_focus_callback(GLFWwindow *window, int focused);
    static void cursor_enter_callback(GLFWwindow *window, int entered);
    static void drop_callback(GLFWwindow *window, int count, const char **paths);
};
}// namespace Kataglyphis::Frontend
