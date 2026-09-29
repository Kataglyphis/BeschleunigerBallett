// LeakSanitizer suppressions compiled into commitTestSuite, so every runner of
// the binary (ctest, CI, a bare invocation) gets them without an LSAN_OPTIONS.
//
// The GPU suites call glfwInit() on X11, and libX11's locale setup
// (XSupportsLocale/XSetLocaleModifiers -> _XlcCreateLC, _XlcDefaultMapModifiers)
// allocates state it never frees. Measured 2026-09-29 on the image's llvmpipe
// under Xvfb: every leak report across all 39 GPU tests roots in
// glfwInit -> libX11, none in engine code - and it failed each of them, green
// assertions included. Only libX11 is suppressed; a leak in Src/ still fails.
extern "C" const char *__lsan_default_suppressions();

extern "C" const char *__lsan_default_suppressions() { return "leak:libX11.so\n"; }
