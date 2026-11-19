module;

#include <cmath>

#include <GLFW/glfw3.h>

export module kataglyphis.shared.frontend.frame_input;

export namespace Kataglyphis::Frontend {

/// Clamp a raw frame delta to something a dt-scaled consumer can absorb.
///
/// Pure so it can be unit-tested without GLFW. Two real cases:
/// - `last_time` starts at 0, so the FIRST delta is the entire startup wall
///   clock - window + scene + renderer construction, seconds - and a key held
///   during load lurched the camera by that much in one frame.
/// - A hitch (debugger pause, driver stall) produces the same shape later.
/// Capping at a 10 FPS step turns both into slow motion instead of a
/// teleport. Negative and NaN deltas become zero.
inline float clamp_frame_delta(float raw_delta)
{
    constexpr float kMaxDeltaSeconds = 0.1F;
    // Not `< 0`: a NaN fails every comparison and would reach the camera, as Rust's FlyController::advance guards.
    if (!(raw_delta >= 0.0F)) { return 0.0F; }
    if (raw_delta > kMaxDeltaSeconds) { return kMaxDeltaSeconds; }
    return raw_delta;
}

/// The clamped delta from `last_seconds` to `now_seconds`, which becomes the new `last_seconds`; Rust's
/// FrameClock::tick_at. In double: a float clock rounds the delta to 15.6 ms after 36 hours of uptime.
inline float frame_delta(double now_seconds, double &last_seconds)
{
    // A non-finite reading would poison every later delta, so it is dropped instead.
    if (!std::isfinite(now_seconds)) { return 0.0F; }
    const float delta = clamp_frame_delta(static_cast<float>(now_seconds - last_seconds));
    last_seconds = now_seconds;
    return delta;
}

inline void update_frame_timing(float &delta_time, double &last_time)
{ delta_time = frame_delta(glfwGetTime(), last_time); }

template<typename WindowType, typename CameraType>
inline void process_camera_input(WindowType *window, CameraType *camera, float delta_time)
{
    camera->mouse_control(window->get_x_change(), window->get_y_change());
    camera->key_control(window->get_keys(), delta_time);
}

}// namespace Kataglyphis::Frontend
