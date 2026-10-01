module;

#include "../../shared/frontend/CameraState.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <span>

export module kataglyphis.vulkan.camera;

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

    glm::mat4 calculate_viewmatrix();

    void set_near_plane(float near_plane);
    void set_far_plane(float far_plane);
    void set_fov(float fov);
    void set_camera_position(glm::vec3 new_camera_position);
    // Degrees, as the controller keeps them; front, right and up follow.
    void set_orientation(float yaw, float pitch);

    ~Camera();

  private:
    Kataglyphis::Frontend::CameraState camera_state;

    void update();
};
