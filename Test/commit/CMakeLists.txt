if(NOT
   CMAKE_BUILD_TYPE
   STREQUAL
   "Debug"
   AND NOT (CMAKE_BUILD_TYPE STREQUAL "Release" AND KATAGLYPHIS_RELEASE_COMMIT_TESTS))
  message(
    STATUS
      "Skipping commit tests: build type is '${CMAKE_BUILD_TYPE}', requires Debug (or Release with KATAGLYPHIS_RELEASE_COMMIT_TESTS)."
  )
  return()
endif()

add_subdirectory(VulkanEngine)

# The goldens' frame verdict over two PNGs, for compare-renderer-pixels.sh; no engine, so it builds in seconds.
add_executable(compareRendererFrames tools/compareRendererFrames.cpp)
target_include_directories(compareRendererFrames PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/VulkanEngine)
target_link_libraries(
  compareRendererFrames
  PRIVATE myproject_options
          myproject_warnings
          glm
          stb)
set_target_properties(compareRendererFrames PROPERTIES CXX_SCAN_FOR_MODULES OFF COMPILE_WARNING_AS_ERROR ON)
