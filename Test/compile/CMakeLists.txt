if(NOT
   CMAKE_BUILD_TYPE
   STREQUAL
   "Debug")
  message(STATUS "Skipping compile tests: build type is '${CMAKE_BUILD_TYPE}', requires Debug.")
  return()
endif()

include(${CMAKE_SOURCE_DIR}/Test/cmake/CommonTestBuild.cmake)

# Build trees persist between container builds, so without CONFIGURE_DEPENDS a new test file never compiles.
file(GLOB RENDERER_COMPILE_TEST_FILTER CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

if(MYPROJECT_CMAKE_DEBUG_LOG)
  message(STATUS "Render compile test suite files are: ${RENDERER_COMPILE_TEST_FILTER}")
else()
  message(VERBOSE "Render compile test suite files are: ${RENDERER_COMPILE_TEST_FILTER}")
endif()

set(COMPILE_TEST_SUITE compileTestSuite)

add_executable(${COMPILE_TEST_SUITE})

kataglyphis_configure_common_test_target(${COMPILE_TEST_SUITE})

# Only add test sources - NOT VulkanEngineCore sources
target_sources(${COMPILE_TEST_SUITE} PRIVATE ${RENDERER_COMPILE_TEST_FILTER})
kataglyphis_add_imgui_test_sources(${COMPILE_TEST_SUITE})

# VulkanEngineCore brings the libraries, includes and module BMIs.
target_link_libraries(${COMPILE_TEST_SUITE} PRIVATE VulkanEngineCore gtest_main gtest)

kataglyphis_enable_windows_vulkan_delay_load(${COMPILE_TEST_SUITE})

# Use the project root as working directory so relative paths match the main executable
kataglyphis_configure_gtest_discovery(${COMPILE_TEST_SUITE})
