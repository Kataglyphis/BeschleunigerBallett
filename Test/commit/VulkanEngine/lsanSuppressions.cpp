// Compiled in so no runner needs LSAN_OPTIONS; see docs/gpu-golden-testing.md § Linux CI runs them on llvmpipe.
extern "C" const char *__lsan_default_suppressions();

extern "C" const char *__lsan_default_suppressions() { return "leak:libX11.so\n"; }
