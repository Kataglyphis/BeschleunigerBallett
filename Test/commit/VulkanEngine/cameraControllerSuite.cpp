// Guards CameraControllerState's field mapping: a member reorder that rebinds right/up or yaw/pitch fails an axis check.

#include <gtest/gtest.h>
#include <glm/geometric.hpp>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include <array>
#include <cmath>
#include <glm/glm.hpp>
#include <span>

import kataglyphis.shared.frontend.camera_controller;

namespace {

constexpr float kEpsilon = 1e-4F;

void expect_vec3_near(const glm::vec3 &actual, const glm::vec3 &expected, float epsilon = kEpsilon)
{
    EXPECT_NEAR(actual.x, expected.x, epsilon);
    EXPECT_NEAR(actual.y, expected.y, epsilon);
    EXPECT_NEAR(actual.z, expected.z, epsilon);
}

// Stand-in for CameraState: each test owns the storage that `state(rig)` binds a reference aggregate to.
struct Rig
{
    glm::vec3 position{};
    glm::vec3 front{};
    glm::vec3 world_up{ 0.0F, 1.0F, 0.0F };
    glm::vec3 right{};
    glm::vec3 up{};
    float yaw = -90.0F;
    float pitch = 0.0F;
    float movement_speed = 10.0F;
    float turn_speed = 0.25F;
};

Kataglyphis::Frontend::CameraControllerState state(Rig &rig)
{
    return { .position = rig.position,
        .front = rig.front,
        .world_up = rig.world_up,
        .right = rig.right,
        .up = rig.up,
        .yaw = rig.yaw,
        .pitch = rig.pitch,
        .movement_speed = rig.movement_speed,
        .turn_speed = rig.turn_speed };
}

}// namespace

TEST(CameraControllerUnit, WKeyMovesAlongFrontAndSAlongMinusFront)
{
    Rig rig;
    Kataglyphis::Frontend::update_camera_vectors(state(rig));

    const glm::vec3 start = rig.position;
    const glm::vec3 front = rig.front;
    constexpr float kDeltaTime = 0.5F;
    const float kStep = rig.movement_speed * kDeltaTime;

    std::array<bool, GLFW_KEY_LAST + 1> keys{};
    keys[GLFW_KEY_W] = true;
    Kataglyphis::Frontend::apply_keyboard_input(state(rig), keys, kDeltaTime);
    expect_vec3_near(rig.position, start + front * kStep);

    keys[GLFW_KEY_W] = false;
    keys[GLFW_KEY_S] = true;
    Kataglyphis::Frontend::apply_keyboard_input(state(rig), keys, kDeltaTime);
    expect_vec3_near(rig.position, start);
}

TEST(CameraControllerUnit, DKeyMovesAlongRightAndAAlongMinusRight)
{
    Rig rig;
    Kataglyphis::Frontend::update_camera_vectors(state(rig));

    const glm::vec3 start = rig.position;
    const glm::vec3 right = rig.right;
    constexpr float kDeltaTime = 0.5F;
    const float kStep = rig.movement_speed * kDeltaTime;

    std::array<bool, GLFW_KEY_LAST + 1> keys{};
    keys[GLFW_KEY_D] = true;
    Kataglyphis::Frontend::apply_keyboard_input(state(rig), keys, kDeltaTime);
    // A right/up rebind would move along `up` (mostly +Y) instead, which this catches.
    expect_vec3_near(rig.position, start + right * kStep);

    keys[GLFW_KEY_D] = false;
    keys[GLFW_KEY_A] = true;
    Kataglyphis::Frontend::apply_keyboard_input(state(rig), keys, kDeltaTime);
    expect_vec3_near(rig.position, start);
}

TEST(CameraControllerUnit, QAndEChangeYawInOppositeDirections)
{
    Rig rig;
    const float start_yaw = rig.yaw;
    constexpr float kDeltaTime = 1.0F;

    std::array<bool, GLFW_KEY_LAST + 1> keys{};
    keys[GLFW_KEY_Q] = true;
    Kataglyphis::Frontend::apply_keyboard_input(state(rig), keys, kDeltaTime);
    EXPECT_LT(rig.yaw, start_yaw);

    rig.yaw = start_yaw;
    keys[GLFW_KEY_Q] = false;
    keys[GLFW_KEY_E] = true;
    Kataglyphis::Frontend::apply_keyboard_input(state(rig), keys, kDeltaTime);
    EXPECT_GT(rig.yaw, start_yaw);
}

TEST(CameraControllerUnit, KeyboardYawRateIsIndependentOfMovementSpeed)
{
    Rig slow_rig;
    slow_rig.movement_speed = 10.0F;
    Rig fast_rig;
    fast_rig.movement_speed = 100.0F;
    constexpr float kDeltaTime = 0.5F;

    std::array<bool, GLFW_KEY_LAST + 1> keys{};
    keys[GLFW_KEY_Q] = true;
    Kataglyphis::Frontend::apply_keyboard_input(state(slow_rig), keys, kDeltaTime);
    Kataglyphis::Frontend::apply_keyboard_input(state(fast_rig), keys, kDeltaTime);

    const float expected_yaw = -90.0F - Kataglyphis::Frontend::kKeyboardTurnDegreesPerSecond * kDeltaTime;
    EXPECT_NEAR(slow_rig.yaw, expected_yaw, kEpsilon);
    EXPECT_NEAR(fast_rig.yaw, expected_yaw, kEpsilon);
}

TEST(CameraControllerUnit, KeyboardYawUpdatesTheBasis)
{
    Rig rig;
    Kataglyphis::Frontend::update_camera_vectors(state(rig));
    const glm::vec3 start_front = rig.front;

    std::array<bool, GLFW_KEY_LAST + 1> keys{};
    keys[GLFW_KEY_Q] = true;
    Kataglyphis::Frontend::apply_keyboard_input(state(rig), keys, 1.0F);

    EXPECT_GT(glm::length(rig.front - start_front), kEpsilon);
}

TEST(CameraControllerUnit, PitchClampsAtPlusMinus89)
{
    Rig rig;
    Kataglyphis::Frontend::apply_mouse_input(state(rig), 0.0F, 100000.0F);

    const float expected_y = std::sin(glm::radians(89.0F));
    EXPECT_NEAR(rig.pitch, 89.0F, kEpsilon);
    EXPECT_NEAR(rig.front.y, expected_y, kEpsilon);

    Kataglyphis::Frontend::apply_mouse_input(state(rig), 0.0F, -200000.0F);
    EXPECT_NEAR(rig.pitch, -89.0F, kEpsilon);
    EXPECT_NEAR(rig.front.y, -expected_y, kEpsilon);
}

TEST(CameraControllerUnit, FrontIsNormalizedAndRightIsPerpendicularToFront)
{
    Rig rig;
    Kataglyphis::Frontend::apply_mouse_input(state(rig), 123.0F, -37.0F);

    EXPECT_NEAR(glm::length(rig.front), 1.0F, kEpsilon);
    EXPECT_NEAR(glm::length(rig.right), 1.0F, kEpsilon);
    EXPECT_NEAR(glm::length(rig.up), 1.0F, kEpsilon);
    EXPECT_NEAR(glm::dot(rig.front, rig.right), 0.0F, kEpsilon);
    EXPECT_NEAR(glm::dot(rig.front, rig.up), 0.0F, kEpsilon);
    EXPECT_NEAR(glm::dot(rig.right, rig.up), 0.0F, kEpsilon);
}

TEST(CameraControllerUnit, AShortKeySpanIsIgnoredRatherThanReadPastTheEnd)
{
    Rig rig;
    Kataglyphis::Frontend::update_camera_vectors(state(rig));
    const glm::vec3 start_position = rig.position;
    const float start_yaw = rig.yaw;

    // Every movement key is at least 65, past this span's end; ASan catches a read there, so they must count as released.
    const std::array<bool, 4> short_keys{ true, true, true, true };
    Kataglyphis::Frontend::apply_keyboard_input(state(rig), short_keys, 1.0F);

    expect_vec3_near(rig.position, start_position);
    EXPECT_NEAR(rig.yaw, start_yaw, kEpsilon);
}
