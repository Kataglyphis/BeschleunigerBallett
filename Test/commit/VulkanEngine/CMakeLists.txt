include(${CMAKE_SOURCE_DIR}/Test/cmake/CommonTestBuild.cmake)

set(COMMIT_TEST_SUITE commitTestSuite)

# CONFIGURE_DEPENDS: build trees persist between container builds, so a new test file would never be globbed.
file(GLOB RENDERER_COMMIT_TEST_FILTER CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

if(MYPROJECT_CMAKE_DEBUG_LOG)
  message(STATUS "Render commit test filter is: ${RENDERER_COMMIT_TEST_FILTER}")
else()
  message(VERBOSE "Render commit test filter is: ${RENDERER_COMMIT_TEST_FILTER}")
endif()

add_executable(${COMMIT_TEST_SUITE})

kataglyphis_configure_common_test_target(${COMMIT_TEST_SUITE})

# Only add test sources - NOT VulkanEngineCore sources
target_sources(${COMMIT_TEST_SUITE} PRIVATE ${RENDERER_COMMIT_TEST_FILTER})
kataglyphis_add_imgui_test_sources(${COMMIT_TEST_SUITE})

# VulkanEngineCore brings the libraries, includes and module BMIs.
target_link_libraries(${COMMIT_TEST_SUITE} PRIVATE VulkanEngineCore gtest gtest_main)

kataglyphis_enable_windows_vulkan_delay_load(${COMMIT_TEST_SUITE})

# Use the project root as working directory so relative paths match the main executable
kataglyphis_configure_gtest_discovery(${COMMIT_TEST_SUITE})
