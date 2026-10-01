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
