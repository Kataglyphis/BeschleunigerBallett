// Compiled in so no runner needs LSAN_OPTIONS; see docs/source/gpu-golden-testing.md § Linux CI runs them on llvmpipe.
extern "C" const char *__lsan_default_suppressions();

// A test that only probes GLFW finds libX11 unloaded by exit, so its leak shows under glfwInit and no X11 frame.
extern "C" const char *__lsan_default_suppressions() { return "leak:libX11.so\nleak:glfwInit\n"; }
