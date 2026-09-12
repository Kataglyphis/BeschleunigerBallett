# This project's build POLICY: which options exist, what they default to, and
# how the reusable modules are composed.
#
# The reusable mechanisms this file used to carry inline were upstreamed to
# ANTfrastructure and are included by name off CMAKE_MODULE_PATH (see the top of
# the root CMakeLists.txt):
#
#   SanitizerSupport      myproject_supports_sanitizers, myproject_default_debug_sanitizers
#   CompilerBuildFlags    myproject_apply_compiler_build_flags + the flag-strip helpers,
#                         including the clang-cl -fms-compatibility-version pin, which
#                         now lives next to the image whose VC Tools version it names
#   ProjectOptionsCommon  the core option list and the per-target dispatch that
#                         AccelerANTgine's ProjectOptions.cmake had drifted into a
#                         near-copy of (hoisted 2026-09-09)
#
# What stays here is what another project would NOT want copied: exceptions
# always off, C++23, C++ modules mandatory (a hard FATAL_ERROR, not a fallback),
# cppcheck and IWYU on by default, and this project's build-type gating.

include(CMakeDependentOption)
include(CheckCXXCompilerFlag)

include(SanitizerSupport)
include(CompilerBuildFlags)
include(ProjectOptionsCommon)

# include() above already fails hard when the module is not on CMAKE_MODULE_PATH.
# This catches the other, quieter failure: a STALE same-named file in this repo's
# own cmake/ directory, which is first on CMAKE_MODULE_PATH and would therefore
# win - loading fine and then leaving every macro below undefined.
if(NOT COMMAND myproject_define_core_options
   OR NOT DEFINED MYPROJECT_PROJECT_OPTIONS_COMMON_VERSION
   OR MYPROJECT_PROJECT_OPTIONS_COMMON_VERSION LESS 1)
  message(FATAL_ERROR "ProjectOptionsCommon resolved to a file that does not provide "
                      "myproject_define_core_options (version >= 1). CMAKE_MODULE_PATH is: ${CMAKE_MODULE_PATH}")
endif()

function(myproject_enable_local_hardening target)
  include(Hardening)
  # Current project behavior always keeps the UBSan minimal runtime disabled,
  # even though older logic computed a value first.
  set(_MYPROJECT_ENABLE_UBSAN_MINIMAL_RUNTIME FALSE)
  myproject_enable_hardening(${target} OFF ${_MYPROJECT_ENABLE_UBSAN_MINIMAL_RUNTIME})
endfunction()

macro(myproject_setup_options)
  option(myproject_ENABLE_HARDENING "Enable hardening" ON)
  option(myproject_ENABLE_COVERAGE "Enable coverage reporting" ON)
  option(myproject_ENABLE_GPROF "Enable profiling with gprof (adds -pg flags)" ON)
  option(myproject_ENABLE_GLOBAL_HARDENING "Enable global hardening" OFF)
  # Exceptions are always disabled for consistent behavior across all builds
  # This avoids /EHs vs /EHs- conflicts and reduces binary size
  # turn off for avoiding potential conflicts with dependencies
  # cmake_dependent_option(
  #   myproject_ENABLE_GLOBAL_HARDENING
  #   "Attempt to push hardening options to built dependencies"
  #   OFF
  #   myproject_ENABLE_HARDENING
  #   OFF)

  myproject_supports_sanitizers()
  myproject_default_debug_sanitizers()

  # cppcheck ON is this project's own call; AccelerANTgine keeps it OFF.
  myproject_define_core_options(
    ASAN_DEFAULT
    ${DEFAULT_ASAN}
    UBSAN_DEFAULT
    ${DEFAULT_UBSAN}
    TSAN_DEFAULT
    OFF
    CPPCHECK_DEFAULT
    ON)

  myproject_mark_core_options_advanced()

endmacro()

macro(myproject_global_options)

  # specify the C/C++ standard
  set(CMAKE_CXX_STANDARD 23)
  set(CMAKE_CXX_STANDARD_REQUIRED True)

  set(CMAKE_C_STANDARD 17)
  set(CMAKE_C_STANDARD_REQUIRED True)

  set(myproject_CXX_SCAN_FOR_MODULES ON)
  set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # Enable C++20 modules for clang-cl by default (requires Clang >= 17)
    # Set MYPROJECT_CLANG_CL_MODULE_SCAN_READY=OFF to disable if issues arise
    if(DEFINED MYPROJECT_CLANG_CL_MODULE_SCAN_READY AND NOT MYPROJECT_CLANG_CL_MODULE_SCAN_READY)
      set(myproject_CXX_SCAN_FOR_MODULES OFF)
      message(STATUS "C++ module scanning explicitly disabled for clang-cl.")
    else()
      set(myproject_CXX_SCAN_FOR_MODULES ON)
      message(STATUS "C++ module scanning enabled for clang-cl.")
    endif()
  endif()

  set(MYPROJECT_CXX_SCAN_FOR_MODULES
      ${myproject_CXX_SCAN_FOR_MODULES}
      CACHE INTERNAL "Global C++ module scan switch for project targets")

  message(STATUS "Global C++ modules scan disabled; project targets can opt in explicitly.")

  myproject_cpp_modules_supported()

  # Policy, not mechanism: this project has no header-based fallback, so an
  # unsupported toolchain is a hard stop rather than a degraded build.
  if(NOT myproject_CPP_MODULES_SUPPORTED)
    message(FATAL_ERROR "This project is configured for C++ modules only. "
                        "Use a module-capable toolchain (Clang >= 17, GCC >= 14, or MSVC >= 19.34 with CMake >= 3.28).")
  endif()

  set(myproject_USE_CPP_MODULES ON)
  message(STATUS "C++ modules are enabled for compiler '${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}'.")

  # set build type specific flags (upstream: CompilerBuildFlags)
  myproject_apply_compiler_build_flags(${myproject_ENABLE_SANITIZER_ADDRESS})

  myproject_set_output_directories()

  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
     AND MSVC
     AND myproject_ENABLE_SANITIZER_ADDRESS)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
  endif()

  myproject_configure_lwyu_and_ipo()

  myproject_supports_sanitizers()

  if(myproject_ENABLE_HARDENING AND myproject_ENABLE_GLOBAL_HARDENING)
    include(Hardening)
    set(_MYPROJECT_ENABLE_UBSAN_MINIMAL_RUNTIME FALSE)
    message(
      "${myproject_ENABLE_HARDENING} ${_MYPROJECT_ENABLE_UBSAN_MINIMAL_RUNTIME} ${myproject_ENABLE_SANITIZER_UNDEFINED}"
    )
    myproject_enable_hardening(myproject_options ON ${_MYPROJECT_ENABLE_UBSAN_MINIMAL_RUNTIME})
  endif()
endmacro()

macro(myproject_local_options)
  myproject_create_option_targets()

  myproject_enable_profiling(myproject_options)

  # Always disable C++ exceptions - /EHs- for MSVC, -fno-exceptions for GCC/Clang
  if(MSVC)
    target_compile_options(myproject_options INTERFACE /EHs-)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(myproject_options INTERFACE -fno-exceptions)
  else()
    message(WARNING "Disabling exceptions is not supported for this compiler.")
  endif()

  # This project applies the sanitizers in every build type, not only Debug.
  myproject_apply_sanitizers(myproject_options)

  myproject_apply_unity_pch_cache(myproject_options)

  if(NOT
     CMAKE_BUILD_TYPE
     STREQUAL
     "Release")
    # No --header-filter: this project's .clang-tidy owns HeaderFilterRegex.
    myproject_apply_static_analysis(myproject_options "")
  endif()

  myproject_apply_warnings_as_errors_linker_check()

  if(myproject_ENABLE_HARDENING AND NOT myproject_ENABLE_GLOBAL_HARDENING)
    myproject_enable_local_hardening(myproject_options)
  endif()

  if(NOT
     CMAKE_BUILD_TYPE
     STREQUAL
     "Release")
    myproject_apply_iwyu(myproject_options)
  endif()

  include(Doxygen)
  enable_doxygen()

  myproject_apply_static_analyzer_flags(myproject_options)

  include(Speedup)

endmacro()
