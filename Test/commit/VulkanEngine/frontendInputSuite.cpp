// ESC-to-close is not covered: it needs a live GLFWwindow, and a null one would hit a real GLFW call.

#include <gtest/gtest.h>

#include <GLFW/glfw3.h>
#include <imgui.h>

#include "shared/frontend/WindowInputState.hpp"

import kataglyphis.shared.frontend.frame_input;
import kataglyphis.shared.frontend.window_input_callbacks;

namespace {
using Kataglyphis::Frontend::clamp_frame_delta;
using Kataglyphis::Frontend::consume_axis_delta;
using Kataglyphis::Frontend::cursor_input_mode_for;
using Kataglyphis::Frontend::handle_focus_lost;
using Kataglyphis::Frontend::handle_key_callback;
using Kataglyphis::Frontend::handle_mouse_button_callback;
using Kataglyphis::Frontend::handle_mouse_callback;
using Kataglyphis::Frontend::reset_window_keys;
using Kataglyphis::Frontend::should_capture_cursor;
using Kataglyphis::Frontend::should_release_cursor;
using Kataglyphis::Frontend::window_key_count;
}// namespace

TEST(FrameInputUnit, FirstFrameStartupSpikeIsClamped)
{
    // last_time starts at 0, so the first raw delta is the whole startup wall clock.
    EXPECT_FLOAT_EQ(clamp_frame_delta(7.3F), 0.1F);
}

TEST(FrameInputUnit, OrdinaryFrameDeltasPassThroughUnchanged)
{
    EXPECT_FLOAT_EQ(clamp_frame_delta(1.0F / 60.0F), 1.0F / 60.0F);
    EXPECT_FLOAT_EQ(clamp_frame_delta(0.1F), 0.1F);// boundary is inclusive
    EXPECT_FLOAT_EQ(clamp_frame_delta(0.0F), 0.0F);
}

TEST(FrameInputUnit, NegativeDeltasBecomeZero)
{
    // A clock adjustment must not run time backwards for dt consumers.
    EXPECT_FLOAT_EQ(clamp_frame_delta(-0.25F), 0.0F);
}

TEST(WindowInputUnit, KeyPressAndReleaseTrackTheArray)
{
    bool keys[window_key_count];
    reset_window_keys(keys);

    handle_key_callback(nullptr, keys, GLFW_KEY_W, GLFW_PRESS);
    EXPECT_TRUE(keys[GLFW_KEY_W]);
    handle_key_callback(nullptr, keys, GLFW_KEY_W, GLFW_RELEASE);
    EXPECT_FALSE(keys[GLFW_KEY_W]);
    // GLFW_REPEAT must not clear a held key.
    handle_key_callback(nullptr, keys, GLFW_KEY_A, GLFW_PRESS);
    handle_key_callback(nullptr, keys, GLFW_KEY_A, GLFW_REPEAT);
    EXPECT_TRUE(keys[GLFW_KEY_A]);
}

TEST(WindowInputUnit, OutOfRangeKeysAreIgnoredNotWritten)
{
    bool keys[window_key_count];
    reset_window_keys(keys);
    // GLFW_KEY_UNKNOWN is -1 and media keys can exceed the array; ASan in CI catches either write.
    handle_key_callback(nullptr, keys, GLFW_KEY_UNKNOWN, GLFW_PRESS);
    handle_key_callback(nullptr, keys, window_key_count + 5, GLFW_PRESS);
    for (int i = 0; i < window_key_count; ++i) {
        ASSERT_FALSE(keys[i]) << "key " << i << " set by an out-of-range event";
    }
}

TEST(WindowInputUnit, FirstMouseMoveDoesNotJumpTheCamera)
{
    float last_x = 0.0F;
    float last_y = 0.0F;
    float x_change = 0.0F;
    float y_change = 0.0F;
    bool first_moved = true;

    // A non-zero first delta would snap the camera from the stale position to the cursor.
    handle_mouse_callback(nullptr, last_x, last_y, x_change, y_change, first_moved, true, 640.0, 360.0);
    EXPECT_FLOAT_EQ(x_change, 0.0F);
    EXPECT_FLOAT_EQ(y_change, 0.0F);
    EXPECT_FALSE(first_moved);

    // y is inverted: screen y grows downward, camera pitch upward.
    handle_mouse_callback(nullptr, last_x, last_y, x_change, y_change, first_moved, true, 650.0, 350.0);
    EXPECT_FLOAT_EQ(x_change, 10.0F);
    EXPECT_FLOAT_EQ(y_change, 10.0F);
}

TEST(WindowInputUnit, MultipleEventsInOneFrameAccumulate)
{
    // Several move events arrive per frame; overwriting instead of adding drops all but the last.
    float last_x = 0.0F;
    float last_y = 0.0F;
    float x_change = 0.0F;
    float y_change = 0.0F;
    bool first_moved = true;

    // Re-seeds, contributing zero.
    handle_mouse_callback(nullptr, last_x, last_y, x_change, y_change, first_moved, true, 640.0, 360.0);

    handle_mouse_callback(nullptr, last_x, last_y, x_change, y_change, first_moved, true, 650.0, 355.0);
    handle_mouse_callback(nullptr, last_x, last_y, x_change, y_change, first_moved, true, 660.0, 350.0);
    handle_mouse_callback(nullptr, last_x, last_y, x_change, y_change, first_moved, true, 670.0, 345.0);
    handle_mouse_callback(nullptr, last_x, last_y, x_change, y_change, first_moved, true, 680.0, 340.0);

    EXPECT_FLOAT_EQ(consume_axis_delta(x_change), 40.0F);
    EXPECT_FLOAT_EQ(consume_axis_delta(y_change), 20.0F);

    // A consume is the frame boundary, so motion must not leak into the next frame's total.
    handle_mouse_callback(nullptr, last_x, last_y, x_change, y_change, first_moved, true, 690.0, 335.0);
    EXPECT_FLOAT_EQ(consume_axis_delta(x_change), 10.0F);
    EXPECT_FLOAT_EQ(consume_axis_delta(y_change), 5.0F);
    EXPECT_FLOAT_EQ(consume_axis_delta(x_change), 0.0F);
    EXPECT_FLOAT_EQ(consume_axis_delta(y_change), 0.0F);
}

TEST(WindowInputUnit, LookModeEntryReSeedsTheMouseOrigin)
{
    // Without a re-seed, the first right-drag of every run snaps by the cursor's absolute position.
    Kataglyphis::Frontend::WindowInputState state;
    state.look_mode_active = true;

    handle_mouse_callback(nullptr,
      state.last_x,
      state.last_y,
      state.x_change,
      state.y_change,
      state.mouse_first_moved,
      state.look_mode_active,
      640.0,
      360.0);
    EXPECT_FLOAT_EQ(state.x_change, 0.0F);
    EXPECT_FLOAT_EQ(state.y_change, 0.0F);
}

TEST(WindowInputUnit, FocusLossEndsLookModeAndReSeedsTheMouseOrigin)
{
    // Stale last_x/last_y after alt-tab would snap the camera by the distance crossed while unfocused.
    bool keys[window_key_count];
    reset_window_keys(keys);
    handle_key_callback(nullptr, keys, GLFW_KEY_W, GLFW_PRESS);
    handle_key_callback(nullptr, keys, GLFW_KEY_A, GLFW_PRESS);

    float last_x = 0.0F;
    float last_y = 0.0F;
    float x_change = 0.0F;
    float y_change = 0.0F;
    bool first_moved = true;
    bool look_mode_active = true;

    handle_mouse_callback(nullptr, last_x, last_y, x_change, y_change, first_moved, look_mode_active, 400.0, 300.0);
    EXPECT_FALSE(first_moved);

    handle_focus_lost(keys, first_moved, look_mode_active);
    for (int i = 0; i < window_key_count; ++i) {
        ASSERT_FALSE(keys[i]) << "key " << i << " still held after focus loss";
    }
    EXPECT_FALSE(look_mode_active) << "focus loss must end look mode, not just reset keys";

    // The cursor-pos callback is always installed, so events keep arriving after the drag ends.
    handle_mouse_callback(nullptr, last_x, last_y, x_change, y_change, first_moved, look_mode_active, 900.0, 50.0);
    EXPECT_FLOAT_EQ(x_change, 0.0F);
    EXPECT_FLOAT_EQ(y_change, 0.0F);

    // Re-entry re-seeds (zero first delta), and the second move proves the axis is not permanently dead.
    look_mode_active = true;
    first_moved = true;
    handle_mouse_callback(nullptr, last_x, last_y, x_change, y_change, first_moved, look_mode_active, 900.0, 50.0);
    EXPECT_FLOAT_EQ(x_change, 0.0F);
    EXPECT_FLOAT_EQ(y_change, 0.0F);

    handle_mouse_callback(nullptr, last_x, last_y, x_change, y_change, first_moved, look_mode_active, 910.0, 60.0);
    EXPECT_FLOAT_EQ(x_change, 10.0F);
    EXPECT_FLOAT_EQ(y_change, -10.0F);
}

TEST(WindowInputUnit, CursorMotionOutsideLookModeProducesNoCameraDelta)
{
    // The ImGui backend needs the cursor-pos callback always installed, so only look_mode_active stops steering.
    float last_x = 0.0F;
    float last_y = 0.0F;
    float x_change = 0.0F;
    float y_change = 0.0F;
    bool first_moved = true;
    bool look_mode_active = false;

    handle_mouse_callback(nullptr, last_x, last_y, x_change, y_change, first_moved, look_mode_active, 200.0, 200.0);
    handle_mouse_callback(nullptr, last_x, last_y, x_change, y_change, first_moved, look_mode_active, 500.0, 50.0);

    EXPECT_FLOAT_EQ(x_change, 0.0F);
    EXPECT_FLOAT_EQ(y_change, 0.0F);
}

TEST(WindowInputUnit, LookModeResumesWithoutSnappingAfterIdleMotion)
{
    // Idle motion must keep re-seeding, or entering look mode reports the whole idle trajectory.
    float last_x = 0.0F;
    float last_y = 0.0F;
    float x_change = 0.0F;
    float y_change = 0.0F;
    bool first_moved = true;
    bool look_mode_active = false;

    handle_mouse_callback(nullptr, last_x, last_y, x_change, y_change, first_moved, look_mode_active, 100.0, 100.0);
    handle_mouse_callback(nullptr, last_x, last_y, x_change, y_change, first_moved, look_mode_active, 800.0, 20.0);

    look_mode_active = true;
    first_moved = true;
    handle_mouse_callback(nullptr, last_x, last_y, x_change, y_change, first_moved, look_mode_active, 800.0, 20.0);
    EXPECT_FLOAT_EQ(x_change, 0.0F);
    EXPECT_FLOAT_EQ(y_change, 0.0F);

    handle_mouse_callback(nullptr, last_x, last_y, x_change, y_change, first_moved, look_mode_active, 810.0, 30.0);
    EXPECT_FLOAT_EQ(x_change, 10.0F);
    EXPECT_FLOAT_EQ(y_change, -10.0F);
}

TEST(WindowInputUnit, CursorCrossingAnImGuiPanelDoesNotJumpTheCamera)
{
    ImGuiContext *ctx = ImGui::CreateContext();
    ASSERT_NE(ctx, nullptr);

    float last_x = 0.0F;
    float last_y = 0.0F;
    float x_change = 0.0F;
    float y_change = 0.0F;
    bool first_moved = true;

    ImGui::GetIO().WantCaptureMouse = false;
    handle_mouse_callback(nullptr, last_x, last_y, x_change, y_change, first_moved, true, 100.0, 100.0);

    // The deltas accumulate, so reset them or a pass could come from an earlier call.
    x_change = 0.0F;
    y_change = 0.0F;
    ImGui::GetIO().WantCaptureMouse = true;
    handle_mouse_callback(nullptr, last_x, last_y, x_change, y_change, first_moved, true, 400.0, 100.0);
    EXPECT_FLOAT_EQ(x_change, 0.0F) << "mouse input leaked past the ImGui capture gate";

    // After capture ends, re-seed rather than difference against the stale pre-panel position.
    x_change = 0.0F;
    ImGui::GetIO().WantCaptureMouse = false;
    handle_mouse_callback(nullptr, last_x, last_y, x_change, y_change, first_moved, true, 410.0, 100.0);
    EXPECT_FLOAT_EQ(x_change, 10.0F);

    ImGui::DestroyContext(ctx);
}

TEST(WindowInputUnit, ConsumeAxisDeltaReturnsAndResets)
{
    float axis = 12.5F;
    EXPECT_FLOAT_EQ(consume_axis_delta(axis), 12.5F);
    EXPECT_FLOAT_EQ(axis, 0.0F);
    // Otherwise the camera keeps rotating forever after one mouse move.
    EXPECT_FLOAT_EQ(consume_axis_delta(axis), 0.0F);
}

TEST(WindowInputUnit, ImGuiCaptureGateSwallowsInput)
{
    // The gate reads the current ImGui context, so build a real one.
    ImGuiContext *ctx = ImGui::CreateContext();
    ASSERT_NE(ctx, nullptr);
    ImGui::GetIO().WantCaptureKeyboard = true;
    ImGui::GetIO().WantCaptureMouse = true;

    bool keys[window_key_count];
    reset_window_keys(keys);
    handle_key_callback(nullptr, keys, GLFW_KEY_W, GLFW_PRESS);
    EXPECT_FALSE(keys[GLFW_KEY_W]) << "keyboard input leaked past the ImGui capture gate";

    float last_x = 0.0F;
    float last_y = 0.0F;
    float x_change = 0.0F;
    float y_change = 0.0F;
    bool first_moved = false;
    last_x = 100.0F;
    handle_mouse_callback(nullptr, last_x, last_y, x_change, y_change, first_moved, true, 200.0, 0.0);
    EXPECT_FLOAT_EQ(x_change, 0.0F) << "mouse input leaked past the ImGui capture gate";

    // With capture off, the same events must flow again.
    ImGui::GetIO().WantCaptureKeyboard = false;
    ImGui::GetIO().WantCaptureMouse = false;
    handle_key_callback(nullptr, keys, GLFW_KEY_W, GLFW_PRESS);
    EXPECT_TRUE(keys[GLFW_KEY_W]);

    ImGui::DestroyContext(ctx);
}

TEST(WindowInputUnit, ReleaseIsHonouredWhileImGuiHasCapture)
{
    // A swallowed release would keep the camera moving forever once focus returns.
    ImGuiContext *ctx = ImGui::CreateContext();
    ASSERT_NE(ctx, nullptr);
    ImGui::GetIO().WantCaptureKeyboard = false;

    bool keys[window_key_count];
    reset_window_keys(keys);
    handle_key_callback(nullptr, keys, GLFW_KEY_W, GLFW_PRESS);
    EXPECT_TRUE(keys[GLFW_KEY_W]);

    ImGui::GetIO().WantCaptureKeyboard = true;
    handle_key_callback(nullptr, keys, GLFW_KEY_W, GLFW_RELEASE);
    EXPECT_FALSE(keys[GLFW_KEY_W]) << "release must fall through the ImGui capture gate";

    ImGui::DestroyContext(ctx);
}

TEST(WindowInputUnit, PressIsStillSwallowedWhileImGuiHasCapture)
{
    ImGuiContext *ctx = ImGui::CreateContext();
    ASSERT_NE(ctx, nullptr);
    ImGui::GetIO().WantCaptureKeyboard = true;

    bool keys[window_key_count];
    reset_window_keys(keys);
    handle_key_callback(nullptr, keys, GLFW_KEY_A, GLFW_PRESS);
    EXPECT_FALSE(keys[GLFW_KEY_A]) << "press must still be gated while ImGui has capture";

    ImGui::DestroyContext(ctx);
}

TEST(WindowInputUnit, CursorCaptureDecisionIgnoresImGuiState)
{
    // Capture is a pure function of button/action, independent of ImGui.
    EXPECT_TRUE(should_capture_cursor(GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS));
    EXPECT_FALSE(should_capture_cursor(GLFW_MOUSE_BUTTON_RIGHT, GLFW_RELEASE));
    EXPECT_FALSE(should_capture_cursor(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS));
}

TEST(WindowInputUnit, OnlyTheRightButtonReleaseEndsLookMode)
{
    EXPECT_TRUE(should_release_cursor(GLFW_MOUSE_BUTTON_RIGHT, GLFW_RELEASE));
    EXPECT_FALSE(should_release_cursor(GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS));
    EXPECT_FALSE(should_release_cursor(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS));
    EXPECT_FALSE(should_release_cursor(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE));
    EXPECT_FALSE(should_release_cursor(GLFW_MOUSE_BUTTON_MIDDLE, GLFW_RELEASE));
}

TEST(WindowInputUnit, CursorInputModeFollowsLookMode)
{
    EXPECT_EQ(cursor_input_mode_for(true), GLFW_CURSOR_DISABLED);
    EXPECT_EQ(cursor_input_mode_for(false), GLFW_CURSOR_NORMAL);

    // Window::mouse_button_callback sets the input mode only on transitions, so a cycle must end at NORMAL.
    bool mouse_first_moved = false;
    bool look_mode_active = false;

    handle_mouse_button_callback(nullptr, mouse_first_moved, look_mode_active, GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS);
    EXPECT_EQ(cursor_input_mode_for(look_mode_active), GLFW_CURSOR_DISABLED);

    handle_mouse_button_callback(nullptr, mouse_first_moved, look_mode_active, GLFW_MOUSE_BUTTON_RIGHT, GLFW_RELEASE);
    EXPECT_EQ(cursor_input_mode_for(look_mode_active), GLFW_CURSOR_NORMAL);

    // A focus loss mid-drag must also end at GLFW_CURSOR_NORMAL.
    handle_mouse_button_callback(nullptr, mouse_first_moved, look_mode_active, GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS);
    bool keys[window_key_count];
    reset_window_keys(keys);
    handle_focus_lost(keys, mouse_first_moved, look_mode_active);
    EXPECT_EQ(cursor_input_mode_for(look_mode_active), GLFW_CURSOR_NORMAL);
}
