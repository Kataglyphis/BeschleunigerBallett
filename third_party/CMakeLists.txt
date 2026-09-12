include(FetchContent)

function(_myproject_collect_targets_recursive dir out_var)
  get_property(_dir_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
  set(_targets ${_dir_targets})

  get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
  foreach(_subdir IN LISTS _subdirs)
    _myproject_collect_targets_recursive("${_subdir}" _subdir_targets)
    list(APPEND _targets ${_subdir_targets})
  endforeach()

  set(${out_var} "${_targets}" PARENT_SCOPE)
endfunction()

function(_myproject_append_link_library target dependency)
  if(NOT TARGET "${target}" OR NOT TARGET "${dependency}")
    return()
  endif()

  get_target_property(_target_type "${target}" TYPE)
  if(NOT _target_type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|EXECUTABLE)$")
    return()
  endif()

  get_target_property(_existing_link_libraries "${target}" LINK_LIBRARIES)
  list(FIND _existing_link_libraries "${dependency}" _dependency_index)
  if(_dependency_index EQUAL -1)
    # Append directly to LINK_LIBRARIES so we can extend third-party targets
    # regardless of whether they used the plain or keyword TLL signature.
    set_property(TARGET "${target}" APPEND PROPERTY LINK_LIBRARIES "${dependency}")
  endif()
endfunction()

function(_myproject_add_system_interface_library target)
  add_library(${target} INTERFACE)
  target_include_directories(${target} SYSTEM INTERFACE ${ARGN})
endfunction()

function(_myproject_set_cache_bool variable value description)
  set(${variable} ${value} CACHE BOOL "${description}")
endfunction()

function(_myproject_force_cache_bool variable value description)
  set(${variable} ${value} CACHE BOOL "${description}" FORCE)
endfunction()

function(_myproject_force_cache_string variable value description)
  set(${variable} "${value}" CACHE STRING "${description}" FORCE)
endfunction()

function(_myproject_collect_external_targets out_var)
  _myproject_collect_targets_recursive("${CMAKE_CURRENT_BINARY_DIR}" _external_targets)
  set(${out_var} "${_external_targets}" PARENT_SCOPE)
endfunction()

function(_myproject_set_msvc_runtime_if_linkable target runtime_library)
  if(NOT TARGET "${target}")
    return()
  endif()

  get_target_property(_target_type "${target}" TYPE)
  if(_target_type STREQUAL "INTERFACE_LIBRARY")
    return()
  endif()

  set_property(TARGET "${target}" PROPERTY MSVC_RUNTIME_LIBRARY "${runtime_library}")
endfunction()

function(_myproject_append_link_library_to_targets dependency)
  if(NOT TARGET "${dependency}")
    return()
  endif()

  foreach(_target IN LISTS ARGN)
    _myproject_append_link_library("${_target}" "${dependency}")
  endforeach()
endfunction()

function(_myproject_append_link_library_to_matching_targets dependency target_regex)
  if(NOT TARGET "${dependency}")
    return()
  endif()

  _myproject_collect_external_targets(_external_targets)
  foreach(_external_target IN LISTS _external_targets)
    if(_external_target MATCHES "${target_regex}")
      _myproject_append_link_library("${_external_target}" "${dependency}")
    endif()
  endforeach()
endfunction()

function(_myproject_configure_windows_antlr_target target)
  if(NOT TARGET "${target}")
    return()
  endif()

  _myproject_set_msvc_runtime_if_linkable("${target}" "MultiThreadedDLL")
  # ANTLR 4.13.2 misses <chrono> in ProfilingATNSimulator.cpp.
  target_compile_options("${target}" PRIVATE /FIchrono)
endfunction()

function(_myproject_find_or_install_cxxbridge out_var)
  find_program(_myproject_cxxbridge cxxbridge PATHS "$ENV{HOME}/.cargo/bin/")
  if(NOT _myproject_cxxbridge OR _myproject_cxxbridge MATCHES "-NOTFOUND$")
    message("Could not find cxxbridge, trying to install with `cargo install cxxbridge-cmd'")
    find_program(_myproject_cargo cargo PATHS "$ENV{HOME}/.cargo/bin/")
    if(NOT _myproject_cargo OR _myproject_cargo MATCHES "-NOTFOUND$")
      message(FATAL_ERROR "Requires cargo available in path, install via rustup https://rustup.rs/")
    endif()
    execute_process(COMMAND ${_myproject_cargo} install cxxbridge-cmd)
    find_program(_myproject_cxxbridge cxxbridge PATHS "$ENV{HOME}/.cargo/bin/")
  endif()

  set(${out_var} "${_myproject_cxxbridge}" PARENT_SCOPE)
endfunction()

function(_myproject_resolve_kompute_vulkan_version out_var)
  if(DEFINED Vulkan_VERSION)
    set(_myproject_kompute_vulkan_version "${Vulkan_VERSION}")
  elseif(DEFINED VULKAN_VERSION_MAJOR AND DEFINED VULKAN_VERSION_MINOR AND DEFINED VULKAN_VERSION_PATCH)
    set(_myproject_kompute_vulkan_version "${VULKAN_VERSION_MAJOR}.${VULKAN_VERSION_MINOR}.${VULKAN_VERSION_PATCH}")
  elseif(DEFINED Vulkan_VERSION_MAJOR AND DEFINED Vulkan_VERSION_MINOR AND DEFINED Vulkan_VERSION_PATCH)
    set(_myproject_kompute_vulkan_version "${Vulkan_VERSION_MAJOR}.${Vulkan_VERSION_MINOR}.${Vulkan_VERSION_PATCH}")
  else()
    set(_myproject_kompute_vulkan_version "1.4.328.1")
  endif()

  set(${out_var} "${_myproject_kompute_vulkan_version}" PARENT_SCOPE)
endfunction()

function(_myproject_resolve_kompute_vulkan_header_tag out_var kompute_vulkan_version)
  string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _myproject_kompute_vulkan_semver "${kompute_vulkan_version}")
  if(_myproject_kompute_vulkan_semver)
    set(_myproject_kompute_header_tag "v${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
  else()
    set(_myproject_kompute_header_tag "v1.3.231")
  endif()

  set(${out_var} "${_myproject_kompute_header_tag}" PARENT_SCOPE)
endfunction()

macro(_myproject_configure_kompute_vulkan)
  set(KOMPUTE_OPT_BUILD_EXAMPLES OFF)
  set(KOMPUTE_OPT_BUILD_TESTS OFF)

  # Determine Vulkan version dynamically, falling back to the previous constant if nothing is detected.
  set(KOMPUTE_OPT_USE_VULKAN_VERSION "" CACHE STRING "Override the Vulkan version Kompute requests.")
  if(KOMPUTE_OPT_USE_VULKAN_VERSION STREQUAL "")
    _myproject_resolve_kompute_vulkan_version(_myproject_kompute_vulkan_version)
    set(KOMPUTE_OPT_USE_VULKAN_VERSION "${_myproject_kompute_vulkan_version}")
  else()
    message(STATUS "Kompute Vulkan version overridden to ${KOMPUTE_OPT_USE_VULKAN_VERSION}")
  endif()

  set(KOMPUTE_OPT_BUILT_IN_VULKAN_HEADER_TAG_OVERRIDE ""
      CACHE STRING "Optional manual override for the built-in Vulkan-Headers tag Kompute uses (for example v1.4.328).")

  if(KOMPUTE_OPT_BUILT_IN_VULKAN_HEADER_TAG_OVERRIDE STREQUAL "")
    _myproject_resolve_kompute_vulkan_header_tag(_myproject_kompute_header_tag "${KOMPUTE_OPT_USE_VULKAN_VERSION}")
    _myproject_force_cache_string(
      KOMPUTE_OPT_BUILT_IN_VULKAN_HEADER_TAG
      "${_myproject_kompute_header_tag}"
      "Resolved Vulkan-Headers tag Kompute uses.")
    message(STATUS "Kompute Vulkan-Headers tag set from detected Vulkan version: ${KOMPUTE_OPT_BUILT_IN_VULKAN_HEADER_TAG}")
  else()
    _myproject_force_cache_string(
      KOMPUTE_OPT_BUILT_IN_VULKAN_HEADER_TAG
      "${KOMPUTE_OPT_BUILT_IN_VULKAN_HEADER_TAG_OVERRIDE}"
      "Resolved Vulkan-Headers tag Kompute uses.")
    message(STATUS "Kompute Vulkan-Headers tag overridden to ${KOMPUTE_OPT_BUILT_IN_VULKAN_HEADER_TAG}")
  endif()

  if(TARGET Vulkan::Headers)
    _myproject_force_cache_bool(KOMPUTE_OPT_USE_BUILT_IN_VULKAN_HEADER OFF "Use Vulkan headers from parent project if already provided.")
    message(STATUS "Using existing Vulkan::Headers target from parent project; disabling Kompute built-in Vulkan-Headers.")
  endif()

  set(KOMPUTE_OPT_DISABLE_VULKAN_VERSION_CHECK ON)

  unset(_myproject_kompute_header_tag)
  unset(_myproject_kompute_vulkan_version)
endmacro()

# Must stay >= the absl_TAG FuzzTest pins in
# third_party/FUZZTEST/cmake/BuildDependencies.cmake — this declaration comes
# first and wins, and older LTS releases lack targets FuzzTest links against
# (e.g. absl::random_mocking_access).
# renovate: datasource=github-tags depName=abseil/abseil-cpp versioning=loose
set(ABSL_TAG 20260526.0)
FetchContent_Declare(
  abseil-cpp
  GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
  GIT_TAG ${ABSL_TAG}
  GIT_SHALLOW 1
)
set(ABSL_PROPAGATE_CXX_STD ON)
set(BUILD_TESTING_OLD "${BUILD_TESTING}")
set(BUILD_TESTING OFF)
FetchContent_MakeAvailable(abseil-cpp)
set(BUILD_TESTING "${BUILD_TESTING_OLD}")

set(_MYPROJECT_WINDOWS_FUZZTEST_ASAN OFF)
if(WIN32 AND MSVC AND KATAGLYPHIS_ENABLE_FUZZTEST_FUZZING_MODE)
  set(_MYPROJECT_WINDOWS_FUZZTEST_ASAN ON)
endif()

if(_MYPROJECT_WINDOWS_FUZZTEST_ASAN)
  add_library(kataglyphis_fuzztest_windows_asan INTERFACE)
  myproject_enable_sanitizers(kataglyphis_fuzztest_windows_asan ON OFF OFF OFF OFF)

  # Keep Windows STL annotation mode consistent across the external fuzz build.
  add_compile_definitions(_DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION _DISABLE_OPTIONAL_ANNOTATION)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
   AND MSVC
   AND myproject_ENABLE_SANITIZER_ADDRESS)
  # Abseil overrides the MSVC runtime selection in its own CMakeLists.
  # Force it back to /MD so its STL mode matches the rest of the Windows ASan build.
  _myproject_collect_external_targets(_external_targets)
  foreach(_external_target IN LISTS _external_targets)
    if(_external_target MATCHES "^absl_")
      _myproject_set_msvc_runtime_if_linkable("${_external_target}" "MultiThreadedDLL")

      if(TARGET kataglyphis_fuzztest_windows_asan)
        _myproject_append_link_library("${_external_target}" kataglyphis_fuzztest_windows_asan)
      endif()
    endif()
  endforeach()
endif()

if(BUILD_TESTING)
  # include google test to project
  FetchContent_Declare(googletest
                       URL https://github.com/google/googletest/archive/56efe3983185e3f37e43415d1afa97e3860f187f.zip)

  # For Windows: Prevent overriding the parent project's compiler/linker settings
  _myproject_force_cache_bool(gtest_force_shared_crt ON "")
  FetchContent_MakeAvailable(googletest)
endif()

_myproject_set_cache_bool(BENCHMARK_ENABLE_TESTING OFF "Disable benchmark testing as we don't need it.")
_myproject_set_cache_bool(BENCHMARK_INSTALL_DOCS OFF "Disable benchmark testing as we don't need it.")
_myproject_set_cache_bool(BENCHMARK_ENABLE_INSTALL OFF "Disable benchmark install to avoid overwriting vendor install.")
_myproject_set_cache_bool(BENCHMARK_USE_BUNDLED_GTEST OFF "Disable the bundled GTest usage.")

if(myproject_DISABLE_EXCEPTIONS)
  _myproject_force_cache_bool(BENCHMARK_ENABLE_EXCEPTIONS OFF "Disable benchmark exceptions to match project configuration.")
else()
  _myproject_force_cache_bool(BENCHMARK_ENABLE_EXCEPTIONS ON "Enable benchmark exceptions to match project configuration.")
endif()

add_subdirectory(GOOGLE_BENCHMARK)

if(BUILD_TESTING
   AND NOT (CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
   AND NOT (CMAKE_BUILD_TYPE STREQUAL "Release"))

  if(CMAKE_CXX_COMPILER_ID MATCHES ".*Clang"
     AND (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR _MYPROJECT_WINDOWS_FUZZTEST_ASAN))
    set(RE2_INSTALL OFF)
    _myproject_force_cache_bool(ANTLR_BUILD_CPP_TESTS OFF "Disable vendored ANTLR runtime tests.")

    message("Adding Fuzztest")

    add_subdirectory(FUZZTEST)

    # FuzzTest at main (ad66c13) does not compile against the abseil LTS it
    # itself pins (20260526.0): fuzzing_bit_gen.h friend-declares
    # absl::random_internal::{DistributionCaller, MockHelpers} without
    # including their headers, and in this LTS bit_gen_ref.h no longer pulls
    # them in transitively. Upstream's Bazel build layers includes differently,
    # which is why their CI does not notice. Broke BOTH lanes here - Linux CI
    # outright, Windows only after a truly fresh rebuild, because the reused
    # container had stale objects compiled against an older abseil.
    #
    # Force-include the two headers into every fuzztest_* target. Applied per
    # TARGET, not per directory: GLFW, SPDLOG and friends are declared later in
    # this same scope, and a directory-scoped flag would hand them a header
    # they have no include path for. Both headers are ordinary guarded
    # headers, so force-including them where they are not needed is harmless.
    # Drop this when upstream adds the includes.
    # Absolute paths on purpose. The relative form compiled under clang-cl's
    # /FI on Windows but failed under clang's -include on Linux CI with "no
    # such file or directory": an absolute path resolves under every compiler,
    # wrapper and cwd, and the headers' own includes still travel the -I chain.
    #
    # The SHELL: prefix is load-bearing on the clang path. Without it, CMake
    # de-duplicates the repeated `-include` flag and emits
    # `-include A.h B.h` - so B.h is parsed as a SECOND input file, and clang
    # dies with "cannot specify -o when generating multiple output files".
    # SHELL: keeps each flag+path group intact and exempt from de-duplication.
    # clang-cl's /FI needs no such help: /FIpath is one token that never
    # collides.
    if(MSVC)
      set(_myproject_fuzztest_absl_fix
          "/FI${abseil-cpp_SOURCE_DIR}/absl/random/internal/distribution_caller.h"
          "/FI${abseil-cpp_SOURCE_DIR}/absl/random/internal/mock_helpers.h")
    else()
      set(_myproject_fuzztest_absl_fix
          "SHELL:-include ${abseil-cpp_SOURCE_DIR}/absl/random/internal/distribution_caller.h"
          "SHELL:-include ${abseil-cpp_SOURCE_DIR}/absl/random/internal/mock_helpers.h")
    endif()
    _myproject_collect_external_targets(_fuzztest_fix_targets)
    foreach(_fuzztest_fix_target IN LISTS _fuzztest_fix_targets)
      if(_fuzztest_fix_target MATCHES "^fuzztest_")
        get_target_property(_fix_target_type "${_fuzztest_fix_target}" TYPE)
        # Only targets that link abseil get the force-include. Any TU that
        # includes the broken header necessarily links absl (it uses absl
        # types), while targets like fuzztest_sha1 link no absl, have no absl
        # include path, and fail to RESOLVE the forced header - which is
        # exactly how the first version of this fix broke the Linux build.
        get_target_property(_fix_target_libs "${_fuzztest_fix_target}" LINK_LIBRARIES)
        if(NOT _fix_target_type STREQUAL "INTERFACE_LIBRARY" AND "${_fix_target_libs}" MATCHES "absl")
          target_compile_options("${_fuzztest_fix_target}" PRIVATE ${_myproject_fuzztest_absl_fix})
        endif()
      endif()
    endforeach()
    unset(_myproject_fuzztest_absl_fix)
    unset(_fuzztest_fix_targets)

    if(_MYPROJECT_WINDOWS_FUZZTEST_ASAN)
      _myproject_append_link_library_to_targets(
        kataglyphis_fuzztest_windows_asan
        antlr_frontend
        backend
        code_generation
        generated_antlr_parser
        grammar_domain_code_generator)

      _myproject_append_link_library_to_matching_targets(kataglyphis_fuzztest_windows_asan "^fuzztest_")

      if(TARGET re2 AND TARGET kataglyphis_fuzztest_windows_asan)
        target_link_libraries(re2 PRIVATE kataglyphis_fuzztest_windows_asan)
      endif()

      foreach(_antlr_target IN ITEMS antlr4_static antlr4_shared)
        _myproject_configure_windows_antlr_target("${_antlr_target}")
      endforeach()
    endif()
  endif()
endif()

FetchContent_Declare(
  GSL
  GIT_REPOSITORY "https://github.com/microsoft/GSL"
  # renovate: datasource=github-tags depName=microsoft/GSL
  GIT_TAG "v4.2.1")
FetchContent_MakeAvailable(GSL)

# set all glfw dependencies
_myproject_force_cache_bool(GLFW_BUILD_DOCS OFF "")
_myproject_force_cache_bool(GLFW_BUILD_TESTS OFF "")
_myproject_force_cache_bool(GLFW_BUILD_EXAMPLES OFF "")

# These wrappers keep third-party headers as SYSTEM includes so our analysis and
# warning settings stay focused on project code.
_myproject_add_system_interface_library(tinyobjloader TINY_OBJ_LOADER)
_myproject_add_system_interface_library(cgltf cgltf)
_myproject_add_system_interface_library(stb STB)
_myproject_add_system_interface_library(imgui IMGUI IMGUI/backends)
_myproject_add_system_interface_library(glm GLM)
_myproject_add_system_interface_library(vma VULKAN_MEMORY_ALLOCATOR/include)
_myproject_add_system_interface_library(ktx KTX/include KTX/other_include/KHR)

add_subdirectory(GLFW)

_myproject_force_cache_bool(SPDLOG_NO_EXCEPTIONS ON "Disable SPDLOG exceptions (required when exceptions are disabled globally)")

add_subdirectory(SPDLOG)

add_subdirectory(NLOHMANN_JSON)

_myproject_force_cache_bool(TOMLPLUSPLUS_BUILD_MODULES ON "Enable tomlplusplus C++ module support")
add_subdirectory(tomlplusplus)

# https://stackoverflow.com/questions/49591804/clang-tidy-cmake-exclude-file-from-check/49591908#49591908
# with this trick IMGUI will be excluded from time consuming static code analysis
# we do not want to analyze imgui ...
# and we will not to all this for our testing executable again :)
add_library(IMGUI OBJECT ${IMGUI_FILTER})

target_link_libraries(
  IMGUI
  PRIVATE ${CMAKE_DL_LIBS}
          Vulkan::Vulkan
          glfw
          imgui)

target_include_directories(IMGUI PRIVATE ${Vulkan_INCLUDE_DIRS})

set_target_properties(IMGUI PROPERTIES CXX_CLANG_TIDY "" CXX_CPPCHECK "")

if(RUST_FEATURES)
  if(WIN32)
    set(Rust_CARGO
        "${CMAKE_SOURCE_DIR}/third_party/ANTfrastructure/windows/scripts/cargo-retry.cmd"
        CACHE FILEPATH "Cargo wrapper used to retry transient Windows file-lock failures" FORCE)
  endif()

  FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG master)
  FetchContent_MakeAvailable(Corrosion)

  _myproject_find_or_install_cxxbridge(CXXBRIDGE)
endif()

option(KATAGLYPHIS_BUILD_KOMPUTE_PLAYGROUND
       "Build the Kompute playground and its kompute dependency (exceptions enabled; not part of the engine)" OFF)

if(WIN32)
  add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()

# Kompute exists solely for Src/KomputePlayground; configuring and building it
# (plus its Vulkan-Headers resolution) in every engine build paid for a demo
# nobody ran by default.
if(KATAGLYPHIS_BUILD_KOMPUTE_PLAYGROUND)
  _myproject_configure_kompute_vulkan()
  add_subdirectory(KOMPUTE)
endif()
