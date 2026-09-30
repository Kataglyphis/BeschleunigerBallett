// App::run()'s exit code, which the validation scripts use to tell a broken run from a window close.

#include <gtest/gtest.h>

#include "app/AppExitCode.hpp"

using Kataglyphis::appExitCode;

TEST(AppExitCodeUnit, CleanRunSucceeds)
{
    EXPECT_EQ(appExitCode(false, false), EXIT_SUCCESS);
}

TEST(AppExitCodeUnit, DeviceLossFails)
{
    EXPECT_EQ(appExitCode(true, false), EXIT_FAILURE);
}

TEST(AppExitCodeUnit, FatalFrameErrorWithoutDeviceLossFails)
{
    // What hasDeviceLost() alone misses: a failed wait or submit that never set device_lost_detected.
    EXPECT_EQ(appExitCode(false, true), EXIT_FAILURE);
}

TEST(AppExitCodeUnit, BothFail)
{
    EXPECT_EQ(appExitCode(true, true), EXIT_FAILURE);
}
