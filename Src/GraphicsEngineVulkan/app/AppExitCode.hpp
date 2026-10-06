#pragma once

#include <cstdlib>

namespace Kataglyphis {
// App::run()'s exit code; free of Vulkan/GLFW types so a test links it without a device.
constexpr int appExitCode(bool deviceLost, bool fatalFrameError)
{ return (deviceLost || fatalFrameError) ? EXIT_FAILURE : EXIT_SUCCESS; }
}// namespace Kataglyphis
