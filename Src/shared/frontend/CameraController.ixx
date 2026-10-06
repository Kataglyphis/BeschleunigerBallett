module;

#include <GLFW/glfw3.h>
#include <glm/geometric.hpp>
#include <glm/glm.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <span>

export module kataglyphis.shared.frontend.camera_controller;

export namespace Kataglyphis::Frontend {

struct CameraControllerState
{
    glm::vec3 &position;
    glm::vec3 &front;
    glm::vec3 const &world_up;
    glm::vec3 &right;
    glm::vec3 &up;
    float &yaw;
    float &pitch;
    float movement_speed;
    float turn_speed;
};

inline void update_camera_vectors(CameraControllerState state)
{
    state.front.x = std::cos(glm::radians(state.pitch)) * std::cos(glm::radians(state.yaw));
    state.front.y = std::sin(glm::radians(state.pitch));
    state.front.z = std::cos(glm::radians(state.pitch)) * std::sin(glm::radians(state.yaw));
    state.front = glm::normalize(state.front);

    state.right = glm::normalize(glm::cross(state.front, state.world_up));
    state.up = glm::normalize(glm::cross(state.right, state.front));
}

// Degrees per second of keyboard-driven yaw. Deliberately not `turn_speed`
// (degrees per pixel of mouse travel - a different unit) and not
// `movement_speed` (world units per second of translation).
inline constexpr float kKeyboardTurnDegreesPerSecond = 90.0F;

inline void apply_keyboard_input(CameraControllerState state, std::span<const bool> keys, float delta_time)
{
    float const velocity = state.movement_speed * delta_time;
    float const turn = kKeyboardTurnDegreesPerSecond * delta_time;

    auto pressed = [keys](
                     int k) { return static_cast<std::size_t>(k) < keys.size() && keys[static_cast<std::size_t>(k)]; };

    if (pressed(GLFW_KEY_W)) { state.position += state.front * velocity; }
    if (pressed(GLFW_KEY_D)) { state.position += state.right * velocity; }
    if (pressed(GLFW_KEY_A)) { state.position += -state.right * velocity; }
    if (pressed(GLFW_KEY_S)) { state.position += -state.front * velocity; }
    if (pressed(GLFW_KEY_Q)) { state.yaw += -turn; }
    if (pressed(GLFW_KEY_E)) { state.yaw += turn; }

    update_camera_vectors(state);
}

inline void apply_mouse_input(CameraControllerState state, float x_change, float y_change)
{
    x_change *= state.turn_speed;
    y_change *= state.turn_speed;

    state.yaw += x_change;
    state.pitch += y_change;// Y-axis aligned for look behavior

    state.pitch = std::min(state.pitch, 89.0F);
    state.pitch = std::max(state.pitch, -89.0F);

    update_camera_vectors(state);
}

}// namespace Kataglyphis::Frontend
