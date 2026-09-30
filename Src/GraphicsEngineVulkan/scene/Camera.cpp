module;

#include <GLFW/glfw3.h>
#include <glm/geometric.hpp>

#include "../../shared/frontend/CameraState.hpp"

#include <algorithm>
#include <cmath>
#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/trigonometric.hpp>
#include <span>

module kataglyphis.vulkan.camera;

import kataglyphis.shared.frontend.camera_controller;

namespace {

// Kept out of Camera so CameraController.ixx's GLFW include stays out of the module interface.
auto controllerState(Kataglyphis::Frontend::CameraState &s) -> Kataglyphis::Frontend::CameraControllerState
{
    return { .position = s.position,
        .front = s.front,
        .world_up = s.world_up,
        .right = s.right,
        .up = s.up,
        .yaw = s.yaw,
        .pitch = s.pitch,
        .movement_speed = s.movement_speed,
        .turn_speed = s.turn_speed };
}

}// namespace

// Debug frames the Dinosaurs scene; its far_plane must stay above shadowDistance, as shadowFar = min(both).
Camera::Camera()
  :
#if NDEBUG
    // Release loads crytek-sponza, which needs the wide-open defaults.
    camera_state{ .position = glm::vec3(0.0F, 2.0F, 0.0F),
        .front = {},
        .world_up = glm::vec3(0.0F, 1.0F, 0.0F),
        .right = {},
        .up = {},
        .yaw = -90.F,
        .pitch = 0.0F,
        .movement_speed = 10.F,
        .turn_speed = 0.25F,
        .near_plane = 0.1F,
        .far_plane = 4000.F,
        .fov = 45.F }
#else
    camera_state{ .position = glm::vec3(0.0F, 6.0F, 26.0F),
        .front = {},
        .world_up = glm::vec3(0.0F, 1.0F, 0.0F),
        .right = {},
        .up = {},
        .yaw = -90.F,
        .pitch = -10.0F,
        .movement_speed = 10.F,
        .turn_speed = 0.25F,
        .near_plane = 0.1F,
        .far_plane = 150.F,
        .fov = 45.F }
#endif
{
    update();
}

void Camera::key_control(std::span<const bool> keys, float delta_time)
{
    Kataglyphis::Frontend::apply_keyboard_input(controllerState(camera_state), keys, delta_time);
    update();
}

void Camera::mouse_control(float x_change, float y_change)
{
    Kataglyphis::Frontend::apply_mouse_input(controllerState(camera_state), x_change, y_change);
}

void Camera::set_near_plane(float near_plane) { camera_state.near_plane = near_plane; }

void Camera::set_far_plane(float far_plane) { camera_state.far_plane = far_plane; }

void Camera::set_fov(float fov) { camera_state.fov = fov; }

void Camera::set_camera_position(glm::vec3 new_camera_position) { camera_state.position = new_camera_position; }

auto Camera::calculate_viewmatrix() -> glm::mat4
{
    // very necessary for further calc
    return glm::lookAt(camera_state.position, camera_state.position + camera_state.front, camera_state.world_up);
}

Camera::~Camera() = default;

void Camera::update() { Kataglyphis::Frontend::update_camera_vectors(controllerState(camera_state)); }
