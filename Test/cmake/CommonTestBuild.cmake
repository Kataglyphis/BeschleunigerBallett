# GoogleTest test registration comes from ContainerHub's GTestDiscovery module
# (kataglyphis_register_gtest_target), which owns the union of what this repo and
# AccelerANTgine had each grown for it: this repo's WORKING_DIRECTORY, and
# AccelerANTgine's clang-cl opt-out plus its add_test/PATH fallback for the
# ASan/UBSan binaries that die during discovery with 0xc0000135.
#
# The module includes GoogleTest itself, so the bare `include(GoogleTest)` that
# stood on this line is gone - one file owning both is what keeps a consumer from
# having to remember the ordering. Resolved BY NAME off CMAKE_MODULE_PATH, which
# the top-level CMakeLists points at third_party/ContainerHub/cmake.
include(GTestDiscovery)

function(kataglyphis_disable_test_warnings test_target)
  if(MSVC)
    target_compile_options(${test_target} PRIVATE /w)
  else()
    target_compile_options(${test_target} PRIVATE -w)
  endif()
endfunction()

function(kataglyphis_configure_common_test_target test_target)
  set_target_properties(${test_target} PROPERTIES CXX_SCAN_FOR_MODULES ${MYPROJECT_CXX_SCAN_FOR_MODULES})
  kataglyphis_disable_test_warnings(${test_target})
endfunction()

function(kataglyphis_add_imgui_test_sources test_target)
  target_sources(
    ${test_target}
    PRIVATE
      # IMGUI object library - excluded from static analysis
      $<TARGET_OBJECTS:IMGUI>)
endfunction()

function(kataglyphis_enable_windows_vulkan_delay_load test_target)
  if(WIN32 AND MSVC)
    target_link_options(${test_target} PRIVATE /DELAYLOAD:vulkan-1.dll)
    target_link_libraries(${test_target} PRIVATE delayimp)
  endif()
endfunction()

# All the registration logic that used to live in this function now lives in
# ContainerHub's kataglyphis_register_gtest_target. What is left here is the one
# thing that is genuinely THIS project's and that the hub module deliberately
# refuses to default: the working directory.
#
# WORKING_DIRECTORY is opt-in upstream because CMAKE_SOURCE_DIR does not mean the
# same thing in every consumer - in a repo that builds as somebody else's
# subproject it is the outer root, and defaulting it on would silently relocate
# the cwd of tests that rely on CTest's default. Here it is correct and
# load-bearing: both suites read resources by paths relative to the repository
# root, exactly like the main executable does.
#
# It stays a named function rather than being inlined at the two call sites so
# that decision has ONE place to change (and so those call sites, which belong to
# a different area of the tree, did not have to be edited to adopt the module).
#
# REMOVED, deliberately: the else-branch that stood in the old body. It guarded
# on KATAGLYPHIS_ENABLE_GTEST_DISCOVERY, set that variable to ON itself two lines
# earlier when undefined, and nothing in this repository - no cache entry, no
# preset, no CI argument, no other CMake file - ever set it. The branch was
# unreachable, and the message it would have printed ("skipping
# gtest_discover_tests") described a state this repo could not enter: a test
# target registered with CTest under NO name at all. The hub module keeps the
# knob and backs it with a REAL fallback (add_test plus, on Windows, the explicit
# PATH the loader needs), and turns discovery off by itself for clang-cl, which
# is the one case that actually needs it.
function(kataglyphis_configure_gtest_discovery test_target)
  kataglyphis_register_gtest_target(${test_target} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endfunction()
