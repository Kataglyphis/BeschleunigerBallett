cmake_minimum_required(VERSION 3.28.0)

# set the project name
file(READ "version.txt" PROJECT_VERSION_STR)
string(STRIP "${PROJECT_VERSION_STR}" PROJECT_VERSION_STR)

project(
  GraphicsEngine
  VERSION ${PROJECT_VERSION_STR}
  DESCRIPTION "Epic GPU accelerated rendering, compute engine using Vulkan and C++,C,Rust"
  HOMEPAGE_URL "https://jonasheinle.de/"
  LANGUAGES NONE)

# Make version available to C++ code globally
add_compile_definitions(PROJECT_VERSION="${PROJECT_VERSION_STR}")

set(AUTHOR "Jonas Heinle")

# Set WINDOWS_CI to OFF by default
# WINDOWS CI is only able to verify successful compiling ... Windows ... lel
# i will stay on linux ...
set(WINDOWS_CI
    OFF
    CACHE BOOL "Enable Windows CI build options")
set(RUST_FEATURES
    ON
    CACHE BOOL "Enable Rust features in our project.")
set(MYPROJECT_CMAKE_DEBUG_LOG
    OFF
    CACHE BOOL "Enable additional verbose CMake configure logging")

set(MYPROJECT_GCC_TOOLCHAIN_PATH
    ""
    CACHE PATH "Optional GCC toolchain root used by Clang and Rust tooling on Linux (for example /opt/gcc-16.2.0)")

# Discover the GCC toolchain root instead of hardcoding it.
#
# The CI presets used to carry "--gcc-toolchain=/opt/gcc-16.1.0" literally, in
# 24 places across six presets. When the build image bumped GCC 16.1.0 -> 16.2.0
# the prefix became /opt/gcc-16.2.0, the flag pointed at a directory that no
# longer existed, and EVERY Linux job died in the compiler-detection try-compile
# with a message that names neither the flag nor the version:
#
#   /usr/bin/x86_64-linux-gnu-ld.bfd: cannot find crtbeginS.o: No such file or directory
#
# (clang could not locate a GCC install under the given prefix, so it emitted
# the crt objects unqualified and left the linker to fail on them.)
#
# Resolution order: explicit -D wins, then MYPROJECT_GCC_TOOLCHAIN_PATH from the
# environment, then GCC_PREFIX (which the build image EXPORTS itself, currently
# /opt/gcc-16.2.0, derived from GCC_VERSION - so it is authoritative inside the
# container), and finally the newest /opt/gcc-* that actually CONTAINS
# crtbeginS.o. That last clause is the point: a prefix which cannot supply the
# startup objects is not a usable toolchain, so it is not selected merely for
# existing. scripts/linux/gcc-toolchain-root.sh implements the same order for
# the non-CMake steps.
if(UNIX AND NOT APPLE AND NOT MYPROJECT_GCC_TOOLCHAIN_PATH)
  if(DEFINED ENV{MYPROJECT_GCC_TOOLCHAIN_PATH})
    set(MYPROJECT_GCC_TOOLCHAIN_PATH "$ENV{MYPROJECT_GCC_TOOLCHAIN_PATH}")
    message(STATUS "GCC toolchain from environment: ${MYPROJECT_GCC_TOOLCHAIN_PATH}")
  elseif(DEFINED ENV{GCC_PREFIX} AND IS_DIRECTORY "$ENV{GCC_PREFIX}")
    set(MYPROJECT_GCC_TOOLCHAIN_PATH "$ENV{GCC_PREFIX}")
    message(STATUS "GCC toolchain from image GCC_PREFIX: ${MYPROJECT_GCC_TOOLCHAIN_PATH}")
  else()
    file(GLOB _gcc_candidates LIST_DIRECTORIES true "/opt/gcc-*")
    list(SORT _gcc_candidates COMPARE NATURAL ORDER DESCENDING)
    foreach(_gcc_candidate IN LISTS _gcc_candidates)
      file(GLOB _gcc_crtbegin "${_gcc_candidate}/lib/gcc/*/*/crtbeginS.o")
      if(_gcc_crtbegin)
        set(MYPROJECT_GCC_TOOLCHAIN_PATH "${_gcc_candidate}")
        message(STATUS "Auto-detected GCC toolchain: ${MYPROJECT_GCC_TOOLCHAIN_PATH}")
        break()
      endif()
    endforeach()
    unset(_gcc_candidates)
    unset(_gcc_candidate)
    unset(_gcc_crtbegin)
  endif()
endif()

# A configured-but-missing prefix must not reach the compiler: clang does not
# error on an unusable --gcc-toolchain, it just fails to find the crt objects
# much later. Fail here, where the message can name the cause.
if(UNIX
   AND NOT APPLE
   AND MYPROJECT_GCC_TOOLCHAIN_PATH
   AND NOT IS_DIRECTORY "${MYPROJECT_GCC_TOOLCHAIN_PATH}")
  message(
    FATAL_ERROR
      "MYPROJECT_GCC_TOOLCHAIN_PATH is set to '${MYPROJECT_GCC_TOOLCHAIN_PATH}', "
      "which does not exist. Passing it to Clang as --gcc-toolchain would fail "
      "later and far less clearly (\"cannot find crtbeginS.o\"). Point it at an "
      "installed GCC prefix, or leave it empty to auto-detect one under /opt.")
endif()

# --gcc-toolchain is a CLANG option. GCC itself rejects it outright:
#   g++: error: unrecognized command-line option '--gcc-toolchain=/opt/gcc-16.2.0'
# This never came up while the flags lived in CMakePresets.json, because only
# the linux-*-clang presets carried them. Applying them from here reaches every
# Linux configure, so the gcc lanes have to be excluded explicitly.
#
# The compiler ID is not known yet (enable_language is below, and the flags must
# be in place before it so the compiler test exercises the real ones), so decide
# from the configured compiler path - which the presets always set - and fall
# back to $ENV{CXX}/$ENV{CC} for a bare `cmake -S . -B build`.
set(_gcc_toolchain_for_clang FALSE)
foreach(_cc_probe
        "${CMAKE_CXX_COMPILER}" "${CMAKE_C_COMPILER}"
        "$ENV{CXX}" "$ENV{CC}")
  if(_cc_probe MATCHES "clang")
    set(_gcc_toolchain_for_clang TRUE)
    break()
  endif()
endforeach()

if(UNIX
   AND NOT APPLE
   AND MYPROJECT_GCC_TOOLCHAIN_PATH
   AND _gcc_toolchain_for_clang)
  set(_GCC_TOOLCHAIN_ROOT "${MYPROJECT_GCC_TOOLCHAIN_PATH}")
  set(_GCC_TOOLCHAIN_LIB "${_GCC_TOOLCHAIN_ROOT}/lib64")
  if(NOT EXISTS "${_GCC_TOOLCHAIN_LIB}")
    set(_GCC_TOOLCHAIN_LIB "${_GCC_TOOLCHAIN_ROOT}/lib")
  endif()

  if(EXISTS "${_GCC_TOOLCHAIN_LIB}")
    message(STATUS "Using GCC toolchain at ${_GCC_TOOLCHAIN_ROOT} for Clang")
    string(APPEND CMAKE_C_FLAGS " --gcc-toolchain=${_GCC_TOOLCHAIN_ROOT}")
    string(APPEND CMAKE_CXX_FLAGS " --gcc-toolchain=${_GCC_TOOLCHAIN_ROOT}")
    string(APPEND CMAKE_EXE_LINKER_FLAGS
           " -L${_GCC_TOOLCHAIN_LIB} -Wl,-rpath,${_GCC_TOOLCHAIN_LIB} --gcc-toolchain=${_GCC_TOOLCHAIN_ROOT}")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS
           " -L${_GCC_TOOLCHAIN_LIB} -Wl,-rpath,${_GCC_TOOLCHAIN_LIB} --gcc-toolchain=${_GCC_TOOLCHAIN_ROOT}")
  else()
    message(WARNING "GCC toolchain found at ${_GCC_TOOLCHAIN_ROOT}, but no lib or lib64 dir exists.")
  endif()
elseif(UNIX AND NOT APPLE AND MYPROJECT_GCC_TOOLCHAIN_PATH)
  message(STATUS
    "GCC toolchain ${MYPROJECT_GCC_TOOLCHAIN_PATH} detected but NOT applied: "
    "--gcc-toolchain is a Clang option and this configure uses "
    "'${CMAKE_CXX_COMPILER}'. GCC finds its own toolchain.")
endif()

enable_language(CXX C)

if(RUST_FEATURES)
  message(STATUS "Rust features are enabled.")
  if(MSVC)
    message(STATUS "On a windows machine you must use MSVC when integrating rust features.")
  endif()
else()
  message(STATUS "Rust features are disabled.")
  message(STATUS "Your are going to use a non MSVC compiler on windows. Therefore no rust integration possible.")
endif()

# Reusable CMake modules live upstream in the ANTfrastructure submodule so other
# projects consume the same ones (see AGENTS.md "Reusable Work Belongs in
# ANTfrastructure"); this repo's own cmake/ keeps only what encodes THIS project's
# choices. Both go on CMAKE_MODULE_PATH so every module is included by NAME -
# `include(Sanitizers)`, not `include(cmake/Sanitizers.cmake)` - which is what
# lets a module live in either directory without its callers caring.
#
# Local first: a project that needs to override an upstream module can drop a
# same-named file in cmake/ and win, without editing ANTfrastructure.
set(KATAGLYPHIS_ANTFRASTRUCTURE_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ANTfrastructure/cmake")
if(NOT EXISTS "${KATAGLYPHIS_ANTFRASTRUCTURE_CMAKE_DIR}/Sanitizers.cmake")
  message(
    FATAL_ERROR
      "ANTfrastructure submodule not checked out: ${KATAGLYPHIS_ANTFRASTRUCTURE_CMAKE_DIR} has no CMake modules. "
      "Run: git submodule update --init --recursive")
endif()
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" "${KATAGLYPHIS_ANTFRASTRUCTURE_CMAKE_DIR}")

include(PreventInSourceBuilds)
include(ProjectOptions)

myproject_setup_options()

myproject_global_options()

myproject_local_options()

if(myproject_ENABLE_COVERAGE
   AND NOT
       CMAKE_BUILD_TYPE
       STREQUAL
       "Release"
   AND CMAKE_CXX_COMPILER_ID MATCHES ".*Clang"
   AND NOT
       CMAKE_CXX_SIMULATE_ID
       STREQUAL
       "MSVC")
  add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
  add_link_options(-fprofile-instr-generate -fcoverage-mapping)
endif()

include(SystemLibDependencies)
include(Src/GraphicsEngineVulkan/cmake/filters/SetExternalLibsFilters.cmake)
add_subdirectory(third_party)

add_subdirectory(Src)

if(BUILD_TESTING)
  include(CTest)
  enable_testing()
  message(STATUS "BUILD_TESTING is ON - enabling CTest.")
  message(STATUS "Tests are enabled.!")
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    foreach(_myproject_debug_test_dir IN ITEMS Test/commit Test/compile Test/fuzz)
      add_subdirectory(${_myproject_debug_test_dir})
    endforeach()
    message(STATUS "Building test suites: commit, compile, fuzz (Debug)")
  elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    add_subdirectory(Test/perf)
    message(STATUS "Building test suite: perf (RelWithDebInfo)")
  else()
    message(
      STATUS
        "Skipping test suites for CMAKE_BUILD_TYPE='${CMAKE_BUILD_TYPE}'. Use Debug for commit/compile/fuzz or RelWithDebInfo for perf."
    )
  endif()
else()
  message(STATUS "BUILD_TESTING is OFF - skipping CTest enablement.")
  message(STATUS "Tests are disabled.!")
endif()

# for correct library output needed
install(
  TARGETS ${PROJECT_NAME}
  CONFIGURATIONS Release
  RUNTIME DESTINATION bin)

set(_myproject_runtime_resource_dirs
    images
    Resources/ShadersSlang/build
    Resources/Textures
    Resources/Models/crytek-sponza)

foreach(_myproject_resource_dir IN LISTS _myproject_runtime_resource_dirs)
  install(DIRECTORY ${_myproject_resource_dir}/ DESTINATION ${_myproject_resource_dir}/)
endforeach()

set(_myproject_imgui_fonts
    third_party/IMGUI/misc/fonts/Cousine-Regular.ttf
    third_party/IMGUI/misc/fonts/DroidSans.ttf
    third_party/IMGUI/misc/fonts/Karla-Regular.ttf
    third_party/IMGUI/misc/fonts/ProggyClean.ttf
    third_party/IMGUI/misc/fonts/ProggyTiny.ttf
    third_party/IMGUI/misc/fonts/Roboto-Medium.ttf)

install(
  FILES ${_myproject_imgui_fonts}
  DESTINATION third_party/IMGUI/misc/fonts/)
if(UNIX AND NOT APPLE)
  install(FILES docs/packaging/GraphicsEngine.desktop DESTINATION usr/share/applications)
  install(
    FILES docs/packaging/GraphicsEngine.appdata.xml
    DESTINATION usr/share/metainfo
    RENAME de.jonasheinle.GraphicsEngine.appdata.xml)
  install(
    FILES images/Engine_logo.png
    DESTINATION usr/share/icons/hicolor/256x256/apps
    RENAME Engine_logo.png)
  install(
    FILES images/Engine_logo.png
    DESTINATION usr/share/icons/hicolor/256x256/apps
    RENAME Engine_logo)
  install(
    FILES images/Engine_logo.png
    DESTINATION .
    RENAME Engine_logo.png)
endif()

include(cmake/CPackOptions.cmake)
