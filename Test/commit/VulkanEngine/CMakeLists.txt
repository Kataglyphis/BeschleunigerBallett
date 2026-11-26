include(${CMAKE_SOURCE_DIR}/Test/cmake/CommonTestBuild.cmake)

set(COMMIT_TEST_SUITE commitTestSuite)

# CONFIGURE_DEPENDS: build trees persist between container builds, so a new test file would never be globbed.
file(
  GLOB
  RENDERER_COMMIT_TEST_FILTER
  CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

if(MYPROJECT_CMAKE_DEBUG_LOG)
  message(STATUS "Render commit test filter is: ${RENDERER_COMMIT_TEST_FILTER}")
else()
  message(VERBOSE "Render commit test filter is: ${RENDERER_COMMIT_TEST_FILTER}")
endif()

# A GCC unit that imports the renderer holds 5-6 GB (clang about 1 GB), so GCC compiles one at a time.
set(COMMIT_TEST_RENDERER_IMPORTERS)
foreach(test_source IN LISTS RENDERER_COMMIT_TEST_FILTER)
  file(STRINGS "${test_source}" renderer_import REGEX "^import kataglyphis\\.vulkan\\.renderer;")
  if(renderer_import)
    list(APPEND COMMIT_TEST_RENDERER_IMPORTERS "${test_source}")
  endif()
endforeach()
list(REMOVE_ITEM RENDERER_COMMIT_TEST_FILTER ${COMMIT_TEST_RENDERER_IMPORTERS})

add_library(commitTestSuiteRendererImporters OBJECT ${COMMIT_TEST_RENDERER_IMPORTERS})
kataglyphis_configure_common_test_sources(commitTestSuiteRendererImporters)
target_link_libraries(commitTestSuiteRendererImporters PRIVATE VulkanEngineCore gtest)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set_property(GLOBAL APPEND PROPERTY JOB_POOLS kataglyphis_heavy_tests=1)
  set_target_properties(commitTestSuiteRendererImporters PROPERTIES JOB_POOL_COMPILE kataglyphis_heavy_tests)
endif()

add_executable(${COMMIT_TEST_SUITE})

kataglyphis_configure_common_test_target(${COMMIT_TEST_SUITE})

# Only add test sources - NOT VulkanEngineCore sources
target_sources(${COMMIT_TEST_SUITE} PRIVATE ${RENDERER_COMMIT_TEST_FILTER}
                                            $<TARGET_OBJECTS:commitTestSuiteRendererImporters>)
kataglyphis_add_imgui_test_sources(${COMMIT_TEST_SUITE})

# VulkanEngineCore brings the libraries, includes and module BMIs.
target_link_libraries(${COMMIT_TEST_SUITE} PRIVATE VulkanEngineCore gtest gtest_main)

kataglyphis_enable_windows_vulkan_delay_load(${COMMIT_TEST_SUITE})

# Use the project root as working directory so relative paths match the main executable
kataglyphis_configure_gtest_discovery(${COMMIT_TEST_SUITE})
