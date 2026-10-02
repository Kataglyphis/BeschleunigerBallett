cmake_minimum_required(VERSION 3.28.0)

# set the project name
file(READ "VERSION.txt" PROJECT_VERSION_STR)
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

set(WINDOWS_CI
    OFF
    CACHE BOOL "Enable Windows CI build options")
set(RUST_FEATURES
    ON
    CACHE BOOL "Enable Rust features in our project.")
set(MYPROJECT_CMAKE_DEBUG_LOG
    OFF
    CACHE BOOL "Enable additional verbose CMake configure logging")

# No --gcc-toolchain: the image's <triple>-clang++.cfg already selects its GCC for a bare clang.
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

# Local cmake/ comes first, so a same-named module there overrides ANTfrastructure's.
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

# The arm64 cross lane builds Release only, and its windows-11-arm job is the only place an arm64 test can run.
option(KATAGLYPHIS_RELEASE_COMMIT_TESTS "Also build the commit suite in Release (the arm64 cross lane)" OFF)
option(KATAGLYPHIS_RELEASE_FUZZ_TESTS "With it, the fuzz targets too, in FuzzTest unit mode without ASan (Windows)" OFF)

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
  elseif(CMAKE_BUILD_TYPE STREQUAL "Release" AND KATAGLYPHIS_RELEASE_COMMIT_TESTS)
    add_subdirectory(Test/commit)
    message(STATUS "Building test suite: commit (Release, KATAGLYPHIS_RELEASE_COMMIT_TESTS)")
    if(KATAGLYPHIS_RELEASE_FUZZ_TESTS)
      add_subdirectory(Test/fuzz)
    endif()
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
