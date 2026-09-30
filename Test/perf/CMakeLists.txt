if(NOT
   CMAKE_BUILD_TYPE
   STREQUAL
   "RelWithDebInfo")
  message(STATUS "Skipping perf tests: build type is '${CMAKE_BUILD_TYPE}', requires RelWithDebInfo.")
  return()
endif()

include(${CMAKE_SOURCE_DIR}/Test/cmake/CommonTestBuild.cmake)
include(cmake/SetTestFilters.cmake)

set(PERF_TEST_SUITE perfTestSuite)

add_executable(${PERF_TEST_SUITE})

kataglyphis_configure_common_test_target(${PERF_TEST_SUITE})

target_sources(${PERF_TEST_SUITE} PRIVATE ${RENDERER_PERF_TEST_FILTER})
kataglyphis_add_imgui_test_sources(${PERF_TEST_SUITE})

target_link_libraries(
  ${PERF_TEST_SUITE}
  PRIVATE VulkanEngineCore
          benchmark::benchmark
          tinyobjloader
          cgltf)

# Gates on the suite running, not on time: wall-clock thresholds are toothless or flaky, so the JSON is compared.
add_test(
  NAME perf_suite_runs
  COMMAND
    ${PERF_TEST_SUITE} --benchmark_min_time=0.05s
    --benchmark_out=${CMAKE_BINARY_DIR}/perf-results.json
    --benchmark_out_format=json
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Large-asset benchmarks run far longer than unit tests.
set_tests_properties(perf_suite_runs PROPERTIES TIMEOUT 900 LABELS "perf")
