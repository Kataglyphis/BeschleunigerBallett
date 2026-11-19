// KATAGLYPHIS_REQUIRE_GPU turns a GPU test's skip into a failure, so a broken Xvfb cannot turn the GPU suites green.

#include "GpuTestGuard.hpp"
#include "ScopedEnvVar.hpp"

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

namespace {

using Kataglyphis::TestSupport::frameCaptureAvailable;
using Kataglyphis::TestSupport::gpuRequired;
using Kataglyphis::TestSupport::ScopedEnvVar;

struct HarnessWithoutCapture
{
    [[nodiscard]] bool supportsFrameCapture() const { return false; }
};

// EXPECT_FATAL_FAILURE runs a statement that names no locals, so the guarded body is a function of its own.
void guardAHarnessWithoutCapture()
{
    const HarnessWithoutCapture harness{};
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
}

}// namespace

TEST(GpuTestGuardUnit, RequireGpuIsSetAndNonEmptyAsOxidantReadsIt)
{
    {
        const ScopedEnvVar unset("KATAGLYPHIS_REQUIRE_GPU", "");
        EXPECT_FALSE(gpuRequired()) << "a developer run without the variable must keep skipping";
    }
    const ScopedEnvVar required("KATAGLYPHIS_REQUIRE_GPU", "1");
    EXPECT_TRUE(gpuRequired());
}

TEST(GpuTestGuardUnit, ARequiredGpuTurnsTheSkipIntoAFailure)
{
    const ScopedEnvVar required("KATAGLYPHIS_REQUIRE_GPU", "1");
    EXPECT_FATAL_FAILURE(guardAHarnessWithoutCapture(), "KATAGLYPHIS_REQUIRE_GPU is set");
}

// A golden's render helper returns a frame, not the test, so its caller's GTEST_SKIP must find a failure already there.
TEST(GpuTestGuardUnit, ARequiredGpuFailsAHelperThatCannotCaptureAFrame)
{
    {
        const ScopedEnvVar unset("KATAGLYPHIS_REQUIRE_GPU", "");
        EXPECT_FALSE(frameCaptureAvailable(HarnessWithoutCapture{})) << "without the variable the helper only skips";
    }
    const ScopedEnvVar required("KATAGLYPHIS_REQUIRE_GPU", "1");
    EXPECT_NONFATAL_FAILURE(frameCaptureAvailable(HarnessWithoutCapture{}), "KATAGLYPHIS_REQUIRE_GPU is set");
}
