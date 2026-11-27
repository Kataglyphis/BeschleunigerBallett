module;

#include "../../shared/frontend/CameraState.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <span>

export module kataglyphis.vulkan.camera;

export import kataglyphis.vulkan.scene_framing;

export class Camera
{
  public:
    Camera();

    void key_control(std::span<const bool> keys, float delta_time);
    void mouse_control(float x_change, float y_change);

    glm::vec3 get_camera_position() const { return camera_state.position; };
    glm::vec3 get_camera_direction() const { return glm::normalize(camera_state.front); };
    glm::vec3 get_up_axis() const { return camera_state.up; };
    glm::vec3 get_right_axis() const { return camera_state.right; };
    float get_near_plane() const { return camera_state.near_plane; };
    float get_far_plane() const { return camera_state.far_plane; };
    float get_fov() const { return camera_state.fov; };
    float get_yaw() const { return camera_state.yaw; };
    float get_pitch() const { return camera_state.pitch; };
    float get_movement_speed() const { return camera_state.movement_speed; };

    /// Where the camera stands and looks now, with the caller's shadow distance, which the camera does not hold.
    Kataglyphis::CameraPose pose(float shadowDistance) const;
    /// The pose the camera was built with, which a scene the framing leaves alone keeps.
    const Kataglyphis::CameraPose &fixed_pose() const { return fixedPose; }
    /// Moves, turns and re-lenses the camera; the pose's shadow distance is the caller's.
    void apply_pose(const Kataglyphis::CameraPose &newPose);

    glm::mat4 calculate_viewmatrix();

    void set_near_plane(float near_plane);
    void set_far_plane(float far_plane);
    void set_fov(float fov);
    // World units per second for W A S D.
    void set_movement_speed(float speed);
    void set_camera_position(glm::vec3 new_camera_position);
    // Degrees, as the controller keeps them; front, right and up follow.
    void set_orientation(float yaw, float pitch);

    ~Camera();

  private:
    Kataglyphis::Frontend::CameraState camera_state;
    Kataglyphis::CameraPose fixedPose;

    void update();
};
