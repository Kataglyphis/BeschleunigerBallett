// Compiled in so no runner needs LSAN_OPTIONS; see docs/source/gpu-golden-testing.md § Linux CI runs them on llvmpipe.
extern "C" const char *__lsan_default_suppressions();

// glfwInit() leaks libX11's locale state, and a driver its one-time setup, unnamed once the loader unloads it.
extern "C" const char *__lsan_default_suppressions() { return "leak:libX11.so\nleak:setup_loader_term_phys_devs\n"; }
